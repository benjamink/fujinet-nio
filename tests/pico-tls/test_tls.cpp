#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/platform_time.h"
#include "../../src/platform/pico/detail/entropy.h"
#include <array>
#include <fstream>
#include <iterator>
#include <string>
static mbedtls_time_t now=1794744000; // 2026-11-15 UTC, deterministic certificate tests
static mbedtls_time_t clock_now(mbedtls_time_t* out) { if(out) *out=now; return now; }
extern "C" mbedtls_ms_time_t mbedtls_ms_time() { return now*1000; }
// No handshake randomness is used by certificate verification; this test-only
// symbol satisfies the linked crypto implementation without reading hardware.
extern "C" int mbedtls_hardware_poll(void*,unsigned char*,std::size_t,std::size_t* n) { *n=0; return -1; }
namespace {
struct Certificate {
    mbedtls_x509_crt crt;
    Certificate() { mbedtls_x509_crt_init(&crt); }
    ~Certificate() { mbedtls_x509_crt_free(&crt); }
    int load(const char* path) {
        std::ifstream file(std::string(NIO_ROOT)+path);
        std::string pem((std::istreambuf_iterator<char>(file)),{});
        return mbedtls_x509_crt_parse(&crt,reinterpret_cast<const unsigned char*>(pem.c_str()),pem.size()+1);
    }
};
}
TEST_CASE("Pico mbedTLS policy verifies chain hostname and validity period") {
    mbedtls_platform_set_time(clock_now); now=1794744000;
    Certificate root,server,unrelated;
    REQUIRE(root.load("/integration-tests/certs/ca.crt")==0);
    REQUIRE(server.load("/integration-tests/certs/server.crt")==0);
    REQUIRE(unrelated.load("/pico2w/certs/isrg-root-x1.pem")==0);
    std::uint32_t flags=0;
    CHECK(mbedtls_x509_crt_verify(&server.crt,&root.crt,nullptr,"localhost",&flags,nullptr,nullptr)==0);
    CHECK(flags==0);
    CHECK(mbedtls_x509_crt_verify(&server.crt,&root.crt,nullptr,"wrong.invalid",&flags,nullptr,nullptr)!=0);
    CHECK((flags&MBEDTLS_X509_BADCERT_CN_MISMATCH)!=0);
    CHECK(mbedtls_x509_crt_verify(&server.crt,&unrelated.crt,nullptr,"localhost",&flags,nullptr,nullptr)!=0);
    CHECK((flags&MBEDTLS_X509_BADCERT_NOT_TRUSTED)!=0);
    now=2208988800; // 2040: certificates expired
    CHECK(mbedtls_x509_crt_verify(&server.crt,&root.crt,nullptr,"localhost",&flags,nullptr,nullptr)!=0);
    CHECK((flags&MBEDTLS_X509_BADCERT_EXPIRED)!=0);
    now=1577836800; // 2020: not yet valid
    CHECK(mbedtls_x509_crt_verify(&server.crt,&root.crt,nullptr,"localhost",&flags,nullptr,nullptr)!=0);
    CHECK((flags&MBEDTLS_X509_BADCERT_FUTURE)!=0);
}
TEST_CASE("Pico entropy fills arbitrary lengths without overwriting adjacent bytes") {
    for(std::size_t n=0;n<33;++n) {
        std::array<unsigned char,40> out; out.fill(0xa5);
        fujinet::platform::pico::detail::fill_entropy(out.data()+1,n,[] { return std::uint64_t{0x0102030405060708}; });
        CHECK(out.front()==0xa5);
        for(std::size_t i=n+1;i<out.size();++i) CHECK(out[i]==0xa5);
        for(std::size_t i=1;i<=n;++i) CHECK(out[i]!=0xa5);
    }
}

#include "pico/time.h"
#include "fujinet/platform/time.h"
TEST_CASE("Pico clock stays invalid until explicitly set and advances monotonically") {
    using namespace fujinet::platform;
    CHECK(unix_time_seconds()==0); CHECK_FALSE(time_is_valid()); CHECK_FALSE(sync_network_time());
    CHECK_FALSE(set_unix_time_seconds(1));
    REQUIRE(set_unix_time_seconds(1794744000)); CHECK(time_is_valid());
    test_monotonic_us+=3500000; CHECK(unix_time_seconds()==1794744003);
    char out[32]; REQUIRE(format_time_utc_iso8601(unix_time_seconds(),out,sizeof(out)));
    CHECK(std::string(out).find("2026-11-15")==0);
}
