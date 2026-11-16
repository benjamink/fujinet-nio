#include "doctest.h"
#include "../src/platform/pico/detail/http_parser.h"
using fujinet::platform::pico::detail::HttpParser;
static std::string consume(HttpParser& p,std::string_view bytes) {
    std::string out;
    for(unsigned char byte:bytes) {
        std::uint8_t body=0; bool produced=false;
        if(!p.push(byte,body,produced)) break;
        if(produced) out.push_back(body);
    }
    return out;
}
TEST_CASE("Pico HTTP parser handles fixed chunked EOF HEAD and interim responses") {
    HttpParser fixed;
    CHECK(consume(fixed,"HTTP/1.1 200 OK\r\nContent-Length: 3\r\nX-A: v\r\n\r\nabc")=="abc");
    CHECK(fixed.done()); CHECK(fixed.headers_ready()); CHECK(fixed.content_length()==3);
    HttpParser chunk;
    CHECK(consume(chunk,"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2;ext=a\r\nab\r\n1\r\nc\r\n0\r\nX-Trailer: v\r\n\r\n")=="abc");
    CHECK(chunk.done()); CHECK(chunk.status()==200); CHECK_FALSE(chunk.content_length());
    HttpParser eof;
    CHECK(consume(eof,"HTTP/1.0 200 OK\r\n\r\nxyz")=="xyz"); CHECK_FALSE(eof.done()); CHECK(eof.finish());
    HttpParser head(true); CHECK(consume(head,"HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\n").empty()); CHECK(head.done());
}
TEST_CASE("Pico HTTP parser rejects ambiguity truncation malformed chunks and unbounded lines") {
    for(const auto* text:{
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\n\n",
        "HTTP/1.1 200 OK\r\n folded: value\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nX\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na!",
        "HTTP/1.1 200 OK\r\nContent-Length: 4294967296\r\n\r\n"}) {
        HttpParser p; consume(p,text); CHECK(p.failed());
    }
    HttpParser short_body; consume(short_body,"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabc"); CHECK_FALSE(short_body.finish());
    HttpParser long_line; consume(long_line,"HTTP/1.1 200 OK\r\nX: "+std::string(9000,'x')); CHECK(long_line.failed());
}
TEST_CASE("Pico HTTP parser accepts the long header lines real servers send, within its bounds") {
    // GitHub answers with a 4.4 KB Content-Security-Policy on one line (5.8 KB of headers in all).
    // A 2 KiB line limit turned that valid response into an error.
    const std::string csp="Content-Security-Policy: default-src 'none'; "+std::string(4393-44,'a');
    HttpParser github;
    CHECK(consume(github,"HTTP/1.1 200 OK\r\nServer: github.com\r\n"+csp+"\r\nSet-Cookie: "+std::string(421,'c')+"\r\nContent-Length: 2\r\n\r\nok")=="ok");
    CHECK_FALSE(github.failed()); CHECK(github.done()); CHECK(github.headers_ready());
    // The edges: a line of exactly 8192 bytes is accepted and one byte more is not.
    HttpParser at_limit; consume(at_limit,"HTTP/1.1 200 OK\r\nX: "+std::string(8192-3,'x')+"\r\n\r\n");
    CHECK_FALSE(at_limit.failed()); CHECK(at_limit.headers_ready());
    HttpParser over_limit; consume(over_limit,"HTTP/1.1 200 OK\r\nX: "+std::string(8192-2,'x')+"\r\n\r\n");
    CHECK(over_limit.failed());
    // Many long lines are bounded by the total: 16 KiB of metadata in all, not per line.
    HttpParser fits; std::string many="HTTP/1.1 200 OK\r\n";
    for(int i=0;i<3;++i) many+="X"+std::to_string(i)+": "+std::string(4000,'y')+"\r\n";
    consume(fits,many+"\r\n"); CHECK_FALSE(fits.failed());
    HttpParser too_much; std::string lots="HTTP/1.1 200 OK\r\n";
    for(int i=0;i<5;++i) lots+="X"+std::to_string(i)+": "+std::string(4000,'y')+"\r\n";
    consume(too_much,lots+"\r\n"); CHECK(too_much.failed());
}
