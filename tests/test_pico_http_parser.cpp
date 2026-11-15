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
    HttpParser long_line; consume(long_line,"HTTP/1.1 200 OK\r\nX: "+std::string(3000,'x')); CHECK(long_line.failed());
}
