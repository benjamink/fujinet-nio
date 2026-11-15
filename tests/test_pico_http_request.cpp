#include "doctest.h"
#include "../src/platform/pico/detail/http_request.h"
using namespace fujinet::platform::pico::detail;
TEST_CASE("Pico HTTP requests validate URLs methods body framing and injection") {
    fujinet::io::NetworkOpenRequest req{}; req.method=2; req.url="https://example.org:8443/api?q=1#fragment"; req.bodyLenHint=5;
    HttpUrl url; std::string headers;
    REQUIRE(build_http_request(req,url,headers)); CHECK(url.tls); CHECK(url.port==8443);
    CHECK(headers.find("POST /api?q=1 HTTP/1.1\r\n") == 0); CHECK(headers.find("Content-Length: 5\r\n")!=std::string::npos);
    req.headers={{"X-Foo","ok\r\nInjected: yes"}}; CHECK_FALSE(build_http_request(req,url,headers));
    req.headers={{"Content-Length","4"}}; CHECK_FALSE(build_http_request(req,url,headers));
    req.headers={{"Transfer-Encoding","chunked"}}; CHECK_FALSE(build_http_request(req,url,headers));
    req.headers.clear(); req.url="https://user:pass@example.org/"; CHECK_FALSE(build_http_request(req,url,headers));
    req.url="http://example.org:65536/"; CHECK_FALSE(build_http_request(req,url,headers));
    req.url="http://example.org/line\nbreak"; CHECK_FALSE(build_http_request(req,url,headers));
    req.url="http://example.org/a/b"; REQUIRE(build_http_request(req,url,headers));
    CHECK(redirect_url(url,"/next")=="http://example.org/next");
    CHECK(redirect_url(url,"c")=="http://example.org/a/c");
}
