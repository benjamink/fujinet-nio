// The firmware's trust code (tls_trust.cpp) against the real mbedTLS, configured exactly as the
// firmware configures it, verifying real certificate chains against the real platform bundle.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "detail/tls_trust_lookup.h"
#include "ca_bundle_data.h"

#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/x509_crt.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace fujinet::platform::pico;
using detail::CaBundle;
using detail::TrustSources;

// The firmware config asks for a hardware entropy source; certificate verification never uses it.
extern "C" int mbedtls_hardware_poll(void*, unsigned char* output, std::size_t length, std::size_t* out_length) {
    std::memset(output, 0, length);
    *out_length = length;
    return 0;
}

namespace {

// 2026-11-16 12:00 UTC, when the chains were captured.
constexpr mbedtls_time_t kCapture = 1794830400;
mbedtls_time_t g_now = kCapture;
mbedtls_time_t fixed_time(mbedtls_time_t* out) { if (out) *out = g_now; return g_now; }

// A counting allocator: proves nothing leaks and measures what a verification costs.
struct Heap {
    std::size_t live = 0, peak = 0, blocks = 0;
    long fail_after = -1; // fail the Nth allocation from now; -1 never
    std::map<void*, std::size_t> sizes;
} heap;

void* counting_calloc(std::size_t n, std::size_t size) {
    if (heap.fail_after == 0) return nullptr;
    if (heap.fail_after > 0) --heap.fail_after;
    void* p = std::calloc(n, size);
    if (!p) return nullptr;
    heap.sizes[p] = n * size;
    heap.live += n * size; ++heap.blocks;
    heap.peak = std::max(heap.peak, heap.live);
    return p;
}
void counting_free(void* p) {
    if (!p) return;
    auto it = heap.sizes.find(p);
    REQUIRE(it != heap.sizes.end()); // a pointer we did not hand out, or a double free
    heap.live -= it->second; --heap.blocks;
    heap.sizes.erase(it);
    std::free(p);
}

struct Setup {
    Setup() {
        mbedtls_platform_set_calloc_free(counting_calloc, counting_free);
        mbedtls_platform_set_time(fixed_time);
    }
} setup;

std::string read_file(const std::string& name) {
    std::ifstream in(std::string(FIXTURES_DIR) + "/" + name, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "missing fixture " << name);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

const CaBundle& platform() {
    static const CaBundle bundle(ca_bundle, ca_bundle_size);
    return bundle;
}

struct Verdict {
    int rc;
    std::uint32_t flags;
    std::size_t peak; // heap above the parsed chain: what the trust lookup and verification add
    bool clean;       // everything freed
};

// Verify a chain file against the given trust sources, the way mbedTLS does during a handshake.
Verdict verify(const std::string& chain_pem, const char* host, const TrustSources& sources, mbedtls_time_t now = kCapture) {
    g_now = now;
    heap.peak = heap.live = 0;
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    REQUIRE(mbedtls_x509_crt_parse(&chain, reinterpret_cast<const unsigned char*>(chain_pem.c_str()), chain_pem.size() + 1) == 0);
    const std::size_t parsed = heap.live;
    heap.peak = heap.live;
    std::uint32_t flags = 0;
    const int rc = mbedtls_x509_crt_verify_with_ca_cb(&chain, detail::ca_lookup, const_cast<TrustSources*>(&sources),
                                                      &mbedtls_x509_crt_profile_default, host, &flags, nullptr, nullptr);
    const std::size_t peak = heap.peak - parsed;
    mbedtls_x509_crt_free(&chain);
    const bool clean = heap.live == 0 && heap.blocks == 0;
    g_now = kCapture;
    return {rc, flags, peak, clean};
}

TrustSources platform_roots() { TrustSources s; s.bundle = &platform(); return s; }

struct Site { const char* file; const char* host; };
const Site kSites[] = {
    {"letsencrypt.pem", "valid-isrgrootx1.letsencrypt.org"}, {"example.pem", "example.com"},
    {"github.pem", "github.com"},                            {"google.pem", "www.google.com"},
    {"cloudflare.pem", "www.cloudflare.com"},                {"amazon.pem", "www.amazon.com"},
    {"microsoft.pem", "www.microsoft.com"},                  {"wikipedia.pem", "en.wikipedia.org"},
    {"mozilla.pem", "www.mozilla.org"},                      {"apple.pem", "www.apple.com"},
    {"python.pem", "www.python.org"},                        {"kernel.pem", "www.kernel.org"},
};

} // namespace

TEST_CASE("the platform bundle is valid and every root in it parses in mbedTLS with the subject the index says") {
    REQUIRE(platform().valid());
    const std::size_t count = platform().count();
    const std::uint8_t* index = ca_bundle + 16;
    const std::uint8_t* data = ca_bundle + 16 + 16 * count;
    std::size_t parse_failures = 0, subject_mismatches = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* e = index + 16 * i;
        const std::uint32_t der_off = e[4] | e[5] << 8 | e[6] << 16 | static_cast<std::uint32_t>(e[7]) << 24;
        const std::size_t der_len = e[8] | e[9] << 8, subj_off = e[10] | e[11] << 8, subj_len = e[12] | e[13] << 8;
        mbedtls_x509_crt crt;
        mbedtls_x509_crt_init(&crt);
        if (mbedtls_x509_crt_parse_der(&crt, data + der_off, der_len) != 0) { ++parse_failures; mbedtls_x509_crt_free(&crt); continue; }
        // The generator's subject offsets are checked against the library's own parse.
        if (crt.subject_raw.len != subj_len || std::memcmp(crt.subject_raw.p, data + der_off + subj_off, subj_len) != 0) ++subject_mismatches;
        mbedtls_x509_crt_free(&crt);
    }
    CHECK(parse_failures == 0);       // the generator keeps only roots this mbedTLS can use
    CHECK(subject_mismatches == 0);
    CHECK(heap.live == 0);
}

TEST_CASE("real server chains verify against the platform bundle") {
    for (const auto& site : kSites) {
        CAPTURE(site.file);
        const auto verdict = verify(read_file(std::string("chains/") + site.file), site.host, platform_roots());
        CHECK(verdict.rc == 0);
        CHECK(verdict.flags == 0);
        CHECK(verdict.clean);
        MESSAGE(site.file << ": heap added by trust lookup and verification " << verdict.peak << " bytes");
        CHECK(verdict.peak < 24 * 1024); // a handful of certificates, not the 114 in the bundle
    }
}

TEST_CASE("a chain that ends at a root the bundle does not hold is not trusted") {
    const auto verdict = verify(read_file("chains/neg-untrusted.pem"), "untrusted-root.badssl.com", platform_roots());
    CHECK(verdict.rc != 0);
    CHECK((verdict.flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0);
    CHECK(verdict.clean);
}

TEST_CASE("a trusted chain for the wrong hostname is refused") {
    const auto verdict = verify(read_file("chains/neg-wronghost.pem"), "wrong.host.badssl.com", platform_roots());
    CHECK(verdict.rc != 0);
    CHECK((verdict.flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) != 0);
    CHECK((verdict.flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) == 0); // the trust is fine, only the name is wrong
    // The same chain is fine for the name it is actually valid for.
    CHECK(verify(read_file("chains/neg-wronghost.pem"), "badssl.com", platform_roots()).rc == 0);
}

TEST_CASE("expired and not-yet-valid certificates are refused") {
    for (const auto& site : kSites) {
        CAPTURE(site.file);
        const auto chain = read_file(std::string("chains/") + site.file);
        const auto future = verify(chain, site.host, platform_roots(), 2208988800); // 2040-01-01
        CHECK(future.rc != 0);
        CHECK((future.flags & MBEDTLS_X509_BADCERT_EXPIRED) != 0);
        const auto past = verify(chain, site.host, platform_roots(), 1262304000);   // 2010-01-01
        CHECK(past.rc != 0);
        CHECK((past.flags & MBEDTLS_X509_BADCERT_FUTURE) != 0);
    }
}

TEST_CASE("nothing is trusted when no source is configured") {
    for (const auto& site : kSites) {
        CAPTURE(site.file);
        const auto verdict = verify(read_file(std::string("chains/") + site.file), site.host, TrustSources{});
        CHECK(verdict.rc != 0);
        CHECK((verdict.flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0);
        CHECK(verdict.clean);
    }
}

TEST_CASE("a leaf without its intermediate is not trusted, since only roots are anchors") {
    const auto chain = read_file("chains/github.pem");
    const auto end = chain.find("-----END CERTIFICATE-----") + std::strlen("-----END CERTIFICATE-----\n");
    const auto verdict = verify(chain.substr(0, end), "github.com", platform_roots());
    CHECK(verdict.rc != 0);
    CHECK((verdict.flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0);
}

TEST_CASE("a chain whose intermediate signature was altered is not trusted") {
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    const auto pem = read_file("chains/github.pem");
    REQUIRE(mbedtls_x509_crt_parse(&chain, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1) == 0);
    REQUIRE(chain.next != nullptr);
    std::vector<std::uint8_t> leaf(chain.raw.p, chain.raw.p + chain.raw.len);
    std::vector<std::uint8_t> issuer(chain.next->raw.p, chain.next->raw.p + chain.next->raw.len);
    mbedtls_x509_crt_free(&chain);
    leaf.back() ^= 0x01; // the last byte of the leaf is its signature value: the issuer signed something else
    mbedtls_x509_crt tampered;
    mbedtls_x509_crt_init(&tampered);
    REQUIRE(mbedtls_x509_crt_parse_der(&tampered, leaf.data(), leaf.size()) == 0);
    REQUIRE(mbedtls_x509_crt_parse_der(&tampered, issuer.data(), issuer.size()) == 0);
    std::uint32_t flags = 0;
    auto roots = platform_roots();
    const int rc = mbedtls_x509_crt_verify_with_ca_cb(&tampered, detail::ca_lookup, &roots, &mbedtls_x509_crt_profile_default,
                                                      "github.com", &flags, nullptr, nullptr);
    CHECK(rc != 0);
    CHECK((flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0);
    mbedtls_x509_crt_free(&tampered);
    CHECK(heap.live == 0);
}

TEST_CASE("an extra PEM anchors a chain on its own, and only a root that really signed it") {
    const auto sample = read_file("roots-sample.pem"); // includes ISRG Root X1 and X2 among others
    TrustSources only_extra;
    only_extra.extra_pem = sample.c_str();
    only_extra.extra_pem_len = sample.size() + 1;
    // letsencrypt chains to an ISRG root, which the sample holds.
    CHECK(verify(read_file("chains/letsencrypt.pem"), "valid-isrgrootx1.letsencrypt.org", only_extra).rc == 0);
    // A chain anchored somewhere the sample does not hold is refused.
    const auto other = verify(read_file("chains/neg-untrusted.pem"), "untrusted-root.badssl.com", only_extra);
    CHECK(other.rc != 0);
    CHECK(other.clean);
    // Platform plus extra (the additive test-CA mode) still verifies platform chains.
    TrustSources both = platform_roots();
    both.extra_pem = sample.c_str();
    both.extra_pem_len = sample.size() + 1;
    CHECK(verify(read_file("chains/github.pem"), "github.com", both).rc == 0);
}

TEST_CASE("the lookup offers only the roots for the issuer and frees everything it allocates") {
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    const auto pem = read_file("chains/letsencrypt.pem");
    REQUIRE(mbedtls_x509_crt_parse(&chain, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1) == 0);
    mbedtls_x509_crt* top = &chain;
    while (top->next) top = top->next;
    auto roots = platform_roots();

    mbedtls_x509_crt* candidates = reinterpret_cast<mbedtls_x509_crt*>(0x1);
    REQUIRE(detail::ca_lookup(&roots, top, &candidates) == 0);
    REQUIRE(candidates != nullptr);
    int count = 0;
    for (auto* c = candidates; c; c = c->next) {
        ++count;
        CHECK(c->subject_raw.len == top->issuer_raw.len);
        CHECK(std::memcmp(c->subject_raw.p, top->issuer_raw.p, top->issuer_raw.len) == 0);
    }
    CHECK(count >= 1);
    CHECK(count <= 3);
    mbedtls_x509_crt_free(candidates);
    mbedtls_free(candidates);

    // An issuer the bundle does not know: nothing offered, nothing allocated.
    mbedtls_x509_crt stranger;
    mbedtls_x509_crt_init(&stranger);
    const auto neg = read_file("chains/neg-untrusted.pem");
    REQUIRE(mbedtls_x509_crt_parse(&stranger, reinterpret_cast<const unsigned char*>(neg.c_str()), neg.size() + 1) == 0);
    const std::size_t before = heap.live;
    candidates = reinterpret_cast<mbedtls_x509_crt*>(0x1);
    CHECK(detail::ca_lookup(&roots, stranger.next ? stranger.next : &stranger, &candidates) == 0);
    CHECK(candidates == nullptr);
    CHECK(heap.live == before);

    // Missing inputs are not trusted and do not crash.
    candidates = reinterpret_cast<mbedtls_x509_crt*>(0x1);
    CHECK(detail::ca_lookup(nullptr, top, &candidates) == 0);
    CHECK(candidates == nullptr);
    CHECK(detail::ca_lookup(&roots, nullptr, &candidates) == 0);
    CHECK(candidates == nullptr);
    mbedtls_x509_crt_free(&stranger);
    mbedtls_x509_crt_free(&chain);
    CHECK(heap.live == 0);
}

TEST_CASE("an allocation failure at any point is refused cleanly and leaks nothing") {
    const auto pem = read_file("chains/github.pem");
    for (long fail_at = 0; fail_at < 400; fail_at += 1) {
        g_now = kCapture;
        mbedtls_x509_crt chain;
        mbedtls_x509_crt_init(&chain);
        heap.fail_after = -1;
        REQUIRE(mbedtls_x509_crt_parse(&chain, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1) == 0);
        heap.fail_after = fail_at;
        std::uint32_t flags = 0;
        auto roots = platform_roots();
        const int rc = mbedtls_x509_crt_verify_with_ca_cb(&chain, detail::ca_lookup, &roots, &mbedtls_x509_crt_profile_default,
                                                          "github.com", &flags, nullptr, nullptr);
        const bool exhausted = heap.fail_after > 0; // the failure point was beyond this verification
        heap.fail_after = -1;
        // Either the verification finished and succeeded, or it failed: never "verified" with an allocation missing.
        if (rc == 0) CHECK(flags == 0);
        mbedtls_x509_crt_free(&chain);
        CHECK(heap.live == 0);
        CHECK(heap.blocks == 0);
        if (exhausted) break;
    }
}
