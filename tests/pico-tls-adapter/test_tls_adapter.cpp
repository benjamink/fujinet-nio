#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "lwip/altcp_tls.h"
#include "pico/cyw43_arch.h"
#include <array>
#include "../../src/platform/pico/detail/tls_protocol.h"
#include "fujinet/net/tcp_network_protocol_common.h"
#include "fujinet/net/https_trust_config.h"
#include "fujinet/platform/time.h"
namespace fujinet::platform {
bool time_is_valid() { return tls_fake::valid_time; }
std::uint64_t unix_time_seconds() { return tls_fake::valid_time ? 1794744000 : 0; }
}
namespace fujinet::platform::pico {
int resolve_tls_address(const char*,const char*,ip_addr_t& ip,std::uint16_t& port) { ip.addr=1; port=443; return 0; }
}
namespace fujinet::net {
bool TcpNetworkProtocolCommon::parse_tcp_url(const std::string&,std::string& host,std::uint16_t& port,Options&) { host="example.org"; port=443; return true; }
}
using namespace fujinet;
TEST_CASE("Pico TLS adapter installs hostname and selected trust roots before handshake") {
    tls_fake::valid_time=true; auto p=platform::pico::make_tls_protocol(); io::NetworkOpenRequest r; r.url="tls://example.org:443";
    REQUIRE(p->open(r)==io::StatusCode::Ok); auto* pcb=tls_fake::pcbs.back().get(); CHECK(pcb->ssl.hostname=="example.org");
    CHECK((tls_fake::roots.find("PLATFORM ROOT")!=std::string::npos)==(net::https_trust_policy()!=net::HttpsTrustPolicy::TestCaOnly));
    CHECK((tls_fake::roots.find("BEGIN CERTIFICATE")!=std::string::npos)==(net::https_trust_policy()!=net::HttpsTrustPolicy::PlatformDefault));
    CHECK(pcb->connected(pcb->arg,pcb,ERR_OK)==ERR_OK);
    const std::string bytes(30,'x'); std::uint16_t written=0;
    CHECK(p->write_body(0,reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size(),written)==io::StatusCode::Ok); CHECK(written==17);
    CHECK(p->write_body(0,reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size(),written)==io::StatusCode::Ok); CHECK(pcb->sent.size()==17);
    tls_fake::close_mem=true;p->close();tls_fake::close_mem=false;
    CHECK(pcb->dead); CHECK(pcb->arg==nullptr); CHECK(pcb->recv==nullptr); CHECK(pcb->error==nullptr); CHECK(tls_fake::configs==0);
}
TEST_CASE("Pico TLS adapter refuses unknown time and failed verification") {
    auto p=platform::pico::make_tls_protocol();io::NetworkOpenRequest r;r.url="tls://example.org:443";
    tls_fake::valid_time=false; CHECK(p->open(r)==io::StatusCode::NotReady);tls_fake::valid_time=true;
    REQUIRE(p->open(r)==io::StatusCode::Ok);auto* pcb=tls_fake::pcbs.back().get();pcb->ssl.verify=1;
    CHECK(pcb->connected(pcb->arg,pcb,ERR_OK)==ERR_ABRT);CHECK(pcb->dead);p->poll();
    io::NetworkInfo info;CHECK(p->info(info)==io::StatusCode::IOError);CHECK(tls_fake::configs==0);
}
TEST_CASE("Pico TLS adapter bounds concurrent handshakes and retires timeouts") {
    auto a=platform::pico::make_tls_protocol(),b=platform::pico::make_tls_protocol(),c=platform::pico::make_tls_protocol();
    io::NetworkOpenRequest r;r.url="tls://example.org:443";fake::now=0;tls_fake::valid_time=true;
    REQUIRE(a->open(r)==io::StatusCode::Ok);REQUIRE(b->open(r)==io::StatusCode::Ok);CHECK(c->open(r)==io::StatusCode::DeviceBusy);
    fake::now=15001;a->poll();io::NetworkInfo info;CHECK(a->info(info)==io::StatusCode::Timeout);
    CHECK(c->open(r)==io::StatusCode::Ok);a->close();b->close();c->close();CHECK(tls_fake::configs==0);
}

TEST_CASE("Pico TLS receive preserves credit ownership replay EOF and queued bytes before error") {
    auto protocol=platform::pico::make_tls_protocol();io::NetworkOpenRequest r;r.url="tls://example.org:443";
    tls_fake::valid_time=true;REQUIRE(protocol->open(r)==io::StatusCode::Ok);
    auto* pcb=tls_fake::pcbs.back().get();REQUIRE(pcb->connected(pcb->arg,pcb,ERR_OK)==ERR_OK);
    auto packet=[](std::size_t size) {auto* p=pbuf_alloc(PBUF_TRANSPORT,size,PBUF_RAM);std::memset(p->payload,42,size);return p;};
    cyw43_arch_lwip_begin();auto* full=packet(16384);REQUIRE(pcb->recv(pcb->arg,pcb,full,ERR_OK)==ERR_OK);
    auto* refused=packet(1);const auto freed=fake::freed_pbufs;
    CHECK(pcb->recv(pcb->arg,pcb,refused,ERR_OK)==ERR_MEM);CHECK(fake::freed_pbufs==freed);cyw43_arch_lwip_end();
    std::array<std::uint8_t,4096> bytes{};std::uint16_t count=0;bool eof=false,more=false;
    REQUIRE(protocol->read_body(0,bytes.data(),bytes.size(),count,eof,more)==io::StatusCode::Ok);CHECK(count==4096);CHECK(pcb->credit==4096);
    REQUIRE(protocol->read_body(0,bytes.data(),bytes.size(),count,eof,more)==io::StatusCode::Ok);CHECK(pcb->credit==4096);
    for(unsigned offset=4096;offset<16384;offset+=4096) {REQUIRE(protocol->read_body(offset,bytes.data(),bytes.size(),count,eof,more)==io::StatusCode::Ok);CHECK(count==4096);}
    cyw43_arch_lwip_begin();CHECK(pcb->recv(pcb->arg,pcb,refused,ERR_OK)==ERR_OK);CHECK(pcb->recv(pcb->arg,pcb,nullptr,ERR_OK)==ERR_OK);cyw43_arch_lwip_end();
    REQUIRE(protocol->read_body(16384,bytes.data(),bytes.size(),count,eof,more)==io::StatusCode::Ok);CHECK(count==1);CHECK(eof);CHECK(pcb->credit==16385);
    protocol->close();REQUIRE(protocol->open(r)==io::StatusCode::Ok);pcb=tls_fake::pcbs.back().get();REQUIRE(pcb->connected(pcb->arg,pcb,ERR_OK)==ERR_OK);
    cyw43_arch_lwip_begin();REQUIRE(pcb->recv(pcb->arg,pcb,packet(2),ERR_OK)==ERR_OK);pcb->dead=true;pcb->error(pcb->arg,ERR_RST);cyw43_arch_lwip_end();
    REQUIRE(protocol->read_body(0,bytes.data(),bytes.size(),count,eof,more)==io::StatusCode::Ok);CHECK(count==2);CHECK(pcb->credit==0);
    CHECK(protocol->read_body(2,bytes.data(),bytes.size(),count,eof,more)==io::StatusCode::IOError);
}
TEST_CASE("Pico TLS handshake timeout crosses the 32-bit millisecond boundary safely") {
    auto protocol=platform::pico::make_tls_protocol();io::NetworkOpenRequest r;r.url="tls://example.org:443";
    fake::now=UINT64_C(0xffffffff)-5;tls_fake::valid_time=true;REQUIRE(protocol->open(r)==io::StatusCode::Ok);
    fake::now+=10;protocol->poll();io::NetworkInfo info;CHECK(protocol->info(info)==io::StatusCode::Ok);
    fake::now+=15000;protocol->poll();CHECK(protocol->info(info)==io::StatusCode::Timeout);
}
