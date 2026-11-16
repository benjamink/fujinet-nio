#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "detail/ca_bundle.h"
#include "ca_bundle_data.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using fujinet::platform::pico::detail::CaBundle;

namespace {

using Bytes = std::vector<std::uint8_t>;

void put32(Bytes& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
void put16(Bytes& b, std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); }

// A stand-in certificate: some bytes with the subject Name placed at an offset inside.
struct Cert {
    Bytes der;
    std::size_t subject_off, subject_len;
    Bytes subject() const { return Bytes(der.begin() + subject_off, der.begin() + subject_off + subject_len); }
};

Cert cert(const char* subject, std::uint8_t tag) {
    Cert c;
    c.der = {0x30, 0x20, tag};
    c.subject_off = c.der.size();
    const std::size_t n = std::strlen(subject);
    c.der.insert(c.der.end(), subject, subject + n);
    c.subject_len = n;
    c.der.insert(c.der.end(), {tag, tag, tag});
    return c;
}

std::uint32_t hash_of(const Cert& c) { return CaBundle::hash_subject(c.der.data() + c.subject_off, c.subject_len); }

Bytes make_bundle(std::vector<Cert> certs) {
    std::stable_sort(certs.begin(), certs.end(), [](const Cert& a, const Cert& b) { return hash_of(a) < hash_of(b); });
    Bytes out{'N', 'I', 'O', 'C', 'A', 'B', '0', '1'};
    put32(out, static_cast<std::uint32_t>(certs.size()));
    put32(out, static_cast<std::uint32_t>(16 + 16 * certs.size()));
    std::uint32_t at = 0;
    for (const auto& c : certs) {
        put32(out, hash_of(c)); put32(out, at);
        put16(out, static_cast<std::uint16_t>(c.der.size())); put16(out, static_cast<std::uint16_t>(c.subject_off));
        put16(out, static_cast<std::uint16_t>(c.subject_len)); put16(out, 0);
        at += static_cast<std::uint32_t>(c.der.size());
    }
    for (const auto& c : certs) out.insert(out.end(), c.der.begin(), c.der.end());
    return out;
}

std::vector<Bytes> found(const CaBundle& b, const Bytes& subject) {
    std::vector<Bytes> out;
    b.find_subject(subject.data(), subject.size(), [&](CaBundle::Root r) { out.emplace_back(r.der, r.der + r.der_len); });
    return out;
}

} // namespace

TEST_CASE("a valid bundle finds a root by its subject name and nothing else") {
    const auto a = cert("CN=Alpha Root", 0xA1), b = cert("CN=Beta Root", 0xB2), c = cert("CN=Gamma Root", 0xC3);
    const auto blob = make_bundle({a, b, c});
    CaBundle bundle(blob.data(), blob.size());
    REQUIRE(bundle.valid());
    CHECK(bundle.count() == 3);
    CHECK(found(bundle, a.subject()) == std::vector<Bytes>{a.der});
    CHECK(found(bundle, b.subject()) == std::vector<Bytes>{b.der});
    CHECK(found(bundle, c.subject()) == std::vector<Bytes>{c.der});
    CHECK(found(bundle, Bytes{'C', 'N', '=', 'N', 'o', 'p', 'e'}).empty());
    auto near = a.subject(); near.back() ^= 1;
    CHECK(found(bundle, near).empty());               // one bit off is a different name
    auto longer = a.subject(); longer.push_back('x');
    CHECK(found(bundle, longer).empty());             // a longer name is a different name
    auto shorter = a.subject(); shorter.pop_back();
    CHECK(found(bundle, shorter).empty());
    CHECK(bundle.find_subject(nullptr, 4, [](CaBundle::Root) { FAIL("called"); }) == 0);
    CHECK(bundle.find_subject(a.subject().data(), 0, [](CaBundle::Root) { FAIL("called"); }) == 0);
}

TEST_CASE("two different subjects with the same hash are told apart by their bytes") {
    // The index is a 32-bit hash, so different names can collide, and a lookup must then still
    // return only the root whose name matches. Find a real FNV-1a collision (birthday bound,
    // about 77000 tries) rather than assume the hash is collision free.
    std::map<std::uint32_t, std::string> seen;
    std::string first, second;
    for (unsigned n = 0; n < 2000000 && second.empty(); ++n) {
        const std::string name = "CN=Root " + std::to_string(n);
        const auto h = CaBundle::hash_subject(reinterpret_cast<const std::uint8_t*>(name.data()), name.size());
        auto it = seen.find(h);
        if (it != seen.end()) { first = it->second; second = name; } else seen.emplace(h, name);
    }
    REQUIRE_FALSE(second.empty());
    const auto a = cert(first.c_str(), 0xAA), b = cert(second.c_str(), 0xBB);
    REQUIRE(hash_of(a) == hash_of(b));
    REQUIRE(a.subject() != b.subject());
    const auto blob = make_bundle({a, b});
    CaBundle bundle(blob.data(), blob.size());
    REQUIRE(bundle.valid());
    CHECK(found(bundle, a.subject()) == std::vector<Bytes>{a.der}); // not b as well
    CHECK(found(bundle, b.subject()) == std::vector<Bytes>{b.der});
}

TEST_CASE("cross-signed roots that share a subject are all offered") {
    const auto first = cert("CN=Cross Root", 0x11), second = cert("CN=Cross Root", 0x22), other = cert("CN=Other", 0x33);
    const auto blob = make_bundle({first, other, second});
    CaBundle bundle(blob.data(), blob.size());
    REQUIRE(bundle.valid());
    auto both = found(bundle, first.subject());
    CHECK(both.size() == 2);
    CHECK(std::find(both.begin(), both.end(), first.der) != both.end());
    CHECK(std::find(both.begin(), both.end(), second.der) != both.end());
    CHECK(found(bundle, other.subject()) == std::vector<Bytes>{other.der});
}

TEST_CASE("a malformed bundle is rejected and finds nothing") {
    const auto a = cert("CN=Alpha Root", 0xA1), b = cert("CN=Beta Root", 0xB2);
    const auto good = make_bundle({a, b});
    auto bad = [&](const char* why, Bytes blob) {
        INFO(why);
        CaBundle bundle(blob.data(), blob.size());
        CHECK_FALSE(bundle.valid());
        CHECK(bundle.count() == 0);
        CHECK(found(bundle, a.subject()).empty());
    };
    bad("empty", {});
    bad("header only part", Bytes(good.begin(), good.begin() + 12));
    { auto b2 = good; b2[0] = 'X'; bad("wrong magic", b2); }
    { auto b2 = good; b2[8] = 0; bad("count zero", b2); }
    { auto b2 = good; b2[11] = 0x7f; bad("count larger than the file", b2); }
    { auto b2 = good; b2[12] += 1; bad("data offset is not the end of the index", b2); }
    { Bytes b2(good.begin(), good.end() - 1); bad("truncated last certificate", b2); }
    { auto b2 = good; b2[16 + 4] = 0xff; b2[16 + 5] = 0xff; bad("certificate offset outside the data", b2); }
    { auto b2 = good; b2[16 + 8] = 0; b2[16 + 9] = 0; bad("zero length certificate", b2); }
    { auto b2 = good; b2[16 + 10] = 0xf0; bad("subject offset outside the certificate", b2); }
    { auto b2 = good; b2[16 + 12] = 0xf0; bad("subject longer than the certificate", b2); }
    { auto b2 = good; b2[16 + 12] = 0; b2[16 + 13] = 0; bad("empty subject", b2); }
    { auto b2 = good; b2[16 + 0] ^= 0xff; bad("subject hash does not match its bytes", b2); }
    {   // swap the two index entries: no longer sorted
        auto b2 = good;
        for (int i = 0; i < 16; ++i) std::swap(b2[16 + i], b2[32 + i]);
        bad("index not sorted", b2);
    }
    CHECK_FALSE(CaBundle(nullptr, 0).valid());
}

TEST_CASE("any single corrupted byte is rejected or stays inside the buffer") {
    // Whatever is in flash, a lookup must never hand out a pointer outside the bundle.
    const auto a = cert("CN=Alpha Root", 0xA1), b = cert("CN=Beta Root", 0xB2), c = cert("CN=Gamma Root", 0xC3);
    const auto good = make_bundle({a, b, c});
    std::size_t rejected = 0, survived = 0;
    for (std::size_t at = 0; at < good.size(); ++at) {
        for (int value : {0x00, 0xff, 0x7f, 0x01}) {
            auto blob = good;
            if (blob[at] == value) continue;
            blob[at] = static_cast<std::uint8_t>(value);
            CaBundle bundle(blob.data(), blob.size());
            if (!bundle.valid()) { ++rejected; continue; }
            ++survived;
            for (const auto& wanted : {a, b, c}) {
                const auto subject = wanted.subject();
                bundle.find_subject(subject.data(), subject.size(), [&](CaBundle::Root r) {
                    CHECK(r.der >= blob.data());
                    CHECK(r.der + r.der_len <= blob.data() + blob.size());
                    CHECK(r.der_len > 0);
                });
            }
        }
    }
    CHECK(rejected > survived); // most corruption is caught outright
    CHECK(rejected > 0);
}

TEST_CASE("the generated platform bundle is valid and every root in it can be found by its own subject") {
    using namespace fujinet::platform::pico;
    CaBundle bundle(ca_bundle, ca_bundle_size);
    REQUIRE(bundle.valid());
    CHECK(bundle.count() > 100);                // a trust store that shrank to a handful is a bug
    CHECK(ca_bundle_size < 160 * 1024);         // flash budget
    for (std::size_t i = 0; i < bundle.count(); ++i) {
        const std::uint8_t* entry = ca_bundle + 16 + 16 * i;
        const std::uint32_t der_off = entry[4] | entry[5] << 8 | entry[6] << 16 | static_cast<std::uint32_t>(entry[7]) << 24;
        const std::size_t der_len = entry[8] | entry[9] << 8, subj_off = entry[10] | entry[11] << 8, subj_len = entry[12] | entry[13] << 8;
        const std::uint8_t* der = ca_bundle + 16 + 16 * bundle.count() + der_off;
        bool self = false;
        const auto matches = bundle.find_subject(der + subj_off, subj_len, [&](CaBundle::Root r) { if (r.der == der && r.der_len == der_len) self = true; });
        CHECK(matches >= 1);
        CHECK(self);
    }
}
