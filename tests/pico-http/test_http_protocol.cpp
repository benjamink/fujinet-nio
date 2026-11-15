#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/platform/pico/detail/http_protocol.h"
#include <algorithm>
#include <cstring>
#include <vector>
using namespace fujinet;
using io::StatusCode;
namespace {
struct Exchange {
    std::string response,sent,url;
    std::size_t response_at=0,upload=0;
    bool eof=true,closed=false;
};
class Stream final : public io::INetworkProtocol {
    std::shared_ptr<Exchange> e_;
public:
    explicit Stream(std::shared_ptr<Exchange> e):e_(std::move(e)) {}
    StatusCode open(const io::NetworkOpenRequest& r) override { e_->url=r.url; return StatusCode::Ok; }
    StatusCode write_body(std::uint32_t o,const std::uint8_t* p,std::size_t n,std::uint16_t& w) override {
        REQUIRE(o==e_->sent.size()); w=std::min<std::size_t>(n,17); e_->sent.append(reinterpret_cast<const char*>(p),w); return StatusCode::Ok;
    }
    StatusCode read_body(std::uint32_t o,std::uint8_t* p,std::size_t n,std::uint16_t& r,bool& eof,bool& more) override {
        REQUIRE(o==e_->response_at); r=0; eof=false; more=false;
        const auto end=e_->sent.find("\r\n\r\n");
        if(end==std::string::npos || e_->sent.size()<end+4+e_->upload) return StatusCode::NotReady;
        r=std::min({n,std::size_t(7),e_->response.size()-e_->response_at});
        std::memcpy(p,e_->response.data()+e_->response_at,r); e_->response_at+=r;
        more=e_->response_at<e_->response.size(); eof=!more && e_->eof;
        return r || eof ? StatusCode::Ok : StatusCode::NotReady;
    }
    StatusCode info(io::NetworkInfo&) override { return StatusCode::Ok; }
    void poll() override {}
    void close() override { e_->closed=true; }
};
struct Harness {
    std::vector<std::shared_ptr<Exchange>> exchanges;
    std::uint64_t now=0; std::size_t created=0;
    std::unique_ptr<io::INetworkProtocol> make() {
        return platform::pico::make_http_protocol([this](bool) { REQUIRE(created<exchanges.size()); return std::make_unique<Stream>(exchanges[created++]); },[this] { return now; });
    }
    std::shared_ptr<Exchange> add(std::string response,std::size_t upload=0) {
        auto e=std::make_shared<Exchange>(); e->response=std::move(response); e->upload=upload; exchanges.push_back(e); return e;
    }
};
std::string drain(io::INetworkProtocol& protocol) {
    std::string out; std::uint8_t bytes[101];
    for(unsigned i=0;i<10000;++i) {
        std::uint16_t n=0; bool eof=false,more=false;
        const auto status=protocol.read_body(out.size(),bytes,sizeof(bytes),n,eof,more);
        REQUIRE((status==StatusCode::Ok || status==StatusCode::NotReady));
        out.append(reinterpret_cast<char*>(bytes),n); if(eof) return out;
    }
    FAIL("HTTP did not complete"); return out;
}
}
TEST_CASE("Pico HTTP adapter streams chunked body and allowlisted metadata") {
    Harness h; auto e=h.add("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX-Keep: yes\r\nX-Drop: no\r\n\r\n3\r\nabc\r\n0\r\n\r\n");
    auto protocol=h.make(); io::NetworkOpenRequest request; request.method=1; request.url="http://example.org/test"; request.responseHeaderNamesLower={"x-keep"};
    REQUIRE(protocol->open(request)==StatusCode::Ok); CHECK(drain(*protocol)=="abc");
    io::NetworkInfo info; REQUIRE(protocol->info(info)==StatusCode::Ok); CHECK(info.httpStatus==200); CHECK_FALSE(info.hasContentLength); CHECK(info.headersBlock=="x-keep: yes\r\n");
    CHECK(e->sent.find("GET /test HTTP/1.1\r\n")==0); CHECK(e->closed);
    std::uint8_t bytes[10];std::uint16_t n;bool eof,more;
    CHECK(protocol->read_body(0,bytes,sizeof(bytes),n,eof,more)==StatusCode::Ok); CHECK(n==3); CHECK(eof);
    CHECK(protocol->read_body(1,bytes,sizeof(bytes),n,eof,more)==StatusCode::InvalidRequest);
}
TEST_CASE("Pico HTTP POST uploads partial writes with replay and bounded offsets") {
    Harness h;auto e=h.add("HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok",30);
    auto protocol=h.make(); io::NetworkOpenRequest request;request.method=2;request.url="http://example.org/post";request.bodyLenHint=30;
    REQUIRE(protocol->open(request)==StatusCode::Ok); const std::string body(30,'a');
    std::uint16_t n=0;std::size_t offset=0;
    for(unsigned i=0;i<100 && offset<body.size();++i) {
        const auto status=protocol->write_body(offset,reinterpret_cast<const std::uint8_t*>(body.data()+offset),body.size()-offset,n);
        REQUIRE((status==StatusCode::Ok || status==StatusCode::NotReady));
        if(n) { std::uint16_t retry=0; CHECK(protocol->write_body(offset,reinterpret_cast<const std::uint8_t*>(body.data()+offset),body.size()-offset,retry)==StatusCode::Ok); CHECK(retry==n); }
        offset+=n;
    }
    REQUIRE(offset==30); CHECK(drain(*protocol)=="ok"); CHECK(e->sent.substr(e->sent.size()-30)==body);
    CHECK(e->sent.find("Content-Length: 30\r\n")!=std::string::npos);
}
TEST_CASE("Pico HTTP redirects strip cross-origin credentials and bound loops") {
    Harness h;h.add("HTTP/1.1 302 Found\r\nLocation: http://other.example/final\r\nContent-Length: 0\r\n\r\n");
    auto target=h.add("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx");
    auto protocol=h.make();io::NetworkOpenRequest r;r.method=1;r.flags=2;r.url="http://example.org/";r.headers={{"Authorization","secret"},{"Cookie","session"}};
    REQUIRE(protocol->open(r)==StatusCode::Ok);CHECK(drain(*protocol)=="x");CHECK(h.created==2);
    CHECK(target->sent.find("secret")==std::string::npos);CHECK(target->sent.find("session")==std::string::npos);
    CHECK(target->url=="tcp://other.example:80");
}
TEST_CASE("Pico HTTP body queue backpressure preserves large responses") {
    Harness h;const std::string body(20000,'z');h.add("HTTP/1.1 200 OK\r\nContent-Length: 20000\r\n\r\n"+body);
    auto protocol=h.make();io::NetworkOpenRequest r;r.method=1;r.url="http://example.org/";REQUIRE(protocol->open(r)==StatusCode::Ok);
    for(unsigned i=0;i<100;++i) protocol->poll();
    std::size_t available=0;CHECK(protocol->read_available(available));CHECK(available==8192);
    h.now=50000;protocol->poll();CHECK(drain(*protocol)==body);
}
TEST_CASE("Pico HTTP reports truncated responses and idle timeouts") {
    Harness h;h.add("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabc");auto protocol=h.make();
    io::NetworkOpenRequest r;r.method=1;r.url="http://example.org/";REQUIRE(protocol->open(r)==StatusCode::Ok);
    for(unsigned i=0;i<30;++i) protocol->poll();
    io::NetworkInfo info;CHECK(protocol->info(info)==StatusCode::IOError);
    Harness idle;auto e=idle.add("");e->eof=false;auto second=idle.make();REQUIRE(second->open(r)==StatusCode::Ok);
    for(unsigned i=0;i<30;++i) second->poll();
    idle.now=16000;second->poll();CHECK(second->info(info)==StatusCode::Timeout);
}

TEST_CASE("Pico HTTP full-queue stall does not consume server inactivity budget") {
    Harness h;auto e=h.add("HTTP/1.1 200 OK\r\nContent-Length: 9000\r\n\r\n"+std::string(8192,'a'));
    e->eof=false;auto protocol=h.make();io::NetworkOpenRequest r;r.method=1;r.url="http://example.org/";
    REQUIRE(protocol->open(r)==StatusCode::Ok);
    for(unsigned i=0;i<100;++i) protocol->poll();
    std::size_t available=0;protocol->read_available(available);REQUIRE(available==8192);
    h.now=50000;protocol->poll();
    std::uint8_t buffer[4096];std::uint16_t count=0;bool eof=false,more=false;
    REQUIRE(protocol->read_body(0,buffer,sizeof(buffer),count,eof,more)==StatusCode::Ok);CHECK(count==4096);
    REQUIRE(protocol->read_body(4096,buffer,sizeof(buffer),count,eof,more)==StatusCode::Ok);CHECK(count==4096);
    CHECK(protocol->read_body(8192,buffer,sizeof(buffer),count,eof,more)==StatusCode::NotReady);
    h.now+=15001;protocol->poll();io::NetworkInfo info;CHECK(protocol->info(info)==StatusCode::Timeout);
}

TEST_CASE("Pico HTTP unknown-length upload uses bounded chunks and zero-length commit") {
    Harness h;auto e=h.add("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",13);
    auto protocol=h.make();io::NetworkOpenRequest r;r.method=3;r.flags=4;r.url="http://example.org/put";
    REQUIRE(protocol->open(r)==StatusCode::Ok);
    const std::uint8_t body[]{'a','b','c'};std::uint16_t written=0;
    for(unsigned i=0;i<100 && !written;++i) protocol->write_body(0,body,3,written);
    REQUIRE(written==3);
    StatusCode commit=StatusCode::NotReady;
    for(unsigned i=0;i<100 && commit==StatusCode::NotReady;++i) commit=protocol->write_body(3,nullptr,0,written);
    REQUIRE(commit==StatusCode::Ok);CHECK(drain(*protocol)=="ok");
    CHECK(e->sent.find("Transfer-Encoding: chunked\r\n")!=std::string::npos);
    CHECK(e->sent.substr(e->sent.size()-13)=="3\r\nabc\r\n0\r\n\r\n");
}
