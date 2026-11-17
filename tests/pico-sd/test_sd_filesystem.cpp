#include "doctest.h"
#include "fake_sd_card.h"
#include "detail/sd_filesystem.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>

using namespace fujinet;
using fujinet::platform::pico::detail::SdCard;
using fujinet::platform::pico::detail::format_sd_card;
using fujinet::platform::pico::detail::mount_sd_filesystem;

namespace {

constexpr std::uint32_t kRun = 10000000;
constexpr std::uint64_t kNow = 1794832496ull; // 2026-11-16 12:34:56 UTC
std::uint64_t g_now = kNow;
std::uint64_t clock_fn() { return g_now; }

std::vector<std::uint8_t> pattern(std::size_t n, unsigned seed) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>((i * 7 + seed * 29 + (i >> 9) * 3) & 0xFF);
    return v;
}

struct Rig {
    fakesd::Card card;
    SdCard sd;
    std::unique_ptr<fs::IFileSystem> fs;
    explicit Rig(fakesd::Options o = {}) : card(o), sd(card, kRun) {
        REQUIRE(sd.init());
        REQUIRE(format_sd_card(sd));
        fs = mount_sd_filesystem(sd, &clock_fn);
        REQUIRE(fs);
    }
    bool put(const std::string& path, const std::vector<std::uint8_t>& data) {
        auto f = fs->open(path, "wb");
        return f && f->write(data.data(), data.size()) == data.size() && f->flush();
    }
    std::vector<std::uint8_t> get(const std::string& path) {
        auto f = fs->open(path, "rb");
        std::vector<std::uint8_t> out;
        if (!f) return out;
        std::uint8_t buf[700];
        for (std::size_t n; (n = f->read(buf, sizeof buf)) > 0;) out.insert(out.end(), buf, buf + n);
        return out;
    }
};

} // namespace

TEST_CASE("a formatted card mounts as sd0 with an empty root") {
    Rig r;
    CHECK(r.fs->name() == "sd0");
    CHECK(r.fs->kind() == fs::FileSystemKind::LocalSD);
    std::vector<fs::FileInfo> list;
    REQUIRE(r.fs->listDirectory("/", list));
    CHECK(list.empty());
    CHECK(r.fs->exists("/"));
    CHECK(r.fs->isDirectory("/"));
    CHECK_FALSE(r.fs->exists("/nothing"));
}

TEST_CASE("files: create, read back, overwrite, append, seek, tell") {
    Rig r;
    const auto big = pattern(150 * 1024 + 37, 1);
    REQUIRE(r.put("/big.bin", big));
    CHECK(r.get("/big.bin") == big);

    fs::FileInfo info;
    REQUIRE(r.fs->stat("/big.bin", info));
    CHECK(info.sizeBytes == big.size());
    CHECK_FALSE(info.isDirectory);

    const auto small = pattern(100, 2);
    REQUIRE(r.put("/big.bin", small)); // "wb" truncates
    CHECK(r.get("/big.bin") == small);

    { auto f = r.fs->open("/big.bin", "ab"); REQUIRE(f); const auto more = pattern(50, 3); CHECK(f->write(more.data(), 50) == 50); }
    auto both = small; { const auto more = pattern(50, 3); both.insert(both.end(), more.begin(), more.end()); }
    CHECK(r.get("/big.bin") == both);

    auto f = r.fs->open("/big.bin", "r+b");
    REQUIRE(f);
    REQUIRE(f->seek(120));
    CHECK(f->tell() == 120);
    std::uint8_t b[10];
    CHECK(f->read(b, 10) == 10);
    CHECK(std::equal(b, b + 10, both.begin() + 120));
    CHECK(f->tell() == 130);
    CHECK_FALSE(r.fs->open("/big.bin", "rw")); // not a stdio mode
    CHECK_FALSE(r.fs->open("/missing.bin", "rb"));
    auto ro = r.fs->open("/big.bin", "rb");
    REQUIRE(ro);
    CHECK_FALSE(ro->seek(1000000)); // beyond the end of a read-only file
}

TEST_CASE("directories, nesting, rename, delete, and what is refused") {
    Rig r;
    REQUIRE(r.fs->createDirectory("/games"));
    REQUIRE(r.fs->createDirectory("/games/atari"));
    CHECK_FALSE(r.fs->createDirectory("/games"));           // exists
    CHECK_FALSE(r.fs->createDirectory("/nope/deeper"));      // parent missing
    REQUIRE(r.put("/games/atari/a.atr", pattern(3000, 1)));
    CHECK(r.fs->isDirectory("/games/atari"));
    CHECK_FALSE(r.fs->isDirectory("/games/atari/a.atr"));

    std::vector<fs::FileInfo> list;
    REQUIRE(r.fs->listDirectory("/games/atari", list));
    REQUIRE(list.size() == 1);
    CHECK(list[0].path == "/games/atari/a.atr");
    CHECK(list[0].sizeBytes == 3000);

    CHECK_FALSE(r.fs->removeDirectory("/games"));            // not empty
    CHECK_FALSE(r.fs->removeFile("/games"));                 // a directory
    CHECK_FALSE(r.fs->removeDirectory("/games/atari/a.atr"));// a file
    CHECK_FALSE(r.fs->removeFile("/"));
    CHECK_FALSE(r.fs->removeDirectory("/"));

    REQUIRE(r.fs->rename("/games/atari/a.atr", "/games/b.atr"));
    CHECK_FALSE(r.fs->exists("/games/atari/a.atr"));
    CHECK(r.get("/games/b.atr") == pattern(3000, 1));
    REQUIRE(r.fs->removeDirectory("/games/atari"));
    REQUIRE(r.fs->removeFile("/games/b.atr"));
    REQUIRE(r.fs->removeDirectory("/games"));
    REQUIRE(r.fs->listDirectory("/", list));
    CHECK(list.empty());
}

TEST_CASE("long names, spaces, mixed case and UTF-8 survive; paths that escape are refused") {
    Rig r;
    const std::string longname = "/A Rather Long File Name With Spaces and.Dots.v1.2.final.atr";
    const std::string utf8 = "/caf\xC3\xA9 \xE2\x82\xAC.txt"; // "café €.txt"
    REQUIRE(r.put(longname, pattern(10, 1)));
    REQUIRE(r.put(utf8, pattern(11, 2)));
    std::vector<fs::FileInfo> list;
    REQUIRE(r.fs->listDirectory("/", list));
    std::vector<std::string> names;
    for (auto& e : list) names.push_back(e.path);
    CHECK(std::find(names.begin(), names.end(), longname) != names.end());
    CHECK(std::find(names.begin(), names.end(), utf8) != names.end());
    CHECK(r.get(utf8) == pattern(11, 2));
    CHECK(r.fs->exists("/a rather long file name with spaces and.dots.v1.2.final.atr")); // FAT is case-insensitive

    CHECK_FALSE(r.fs->exists("/../etc"));
    CHECK_FALSE(r.fs->open("/a/../b", "rb"));
    CHECK_FALSE(r.fs->exists(""));
    CHECK_FALSE(r.fs->exists("relative"));
}

TEST_CASE("file times come from the device clock, and read back as UTC") {
    Rig r;
    struct Case { std::uint64_t when; const char* what; };
    const Case cases[] = {
        {kNow, "2026-11-16 12:34:56"},
        {1768446246ull, "2026-01-15 03:04:06"},
        {1772323198ull, "2026-02-28 23:59:58"},
        {1835395200ull, "2028-02-29 00:00:00"},
        {1798761598ull, "2026-12-31 23:59:58"},
        {1803859202ull, "2027-03-01 00:00:02"},
        {1783166400ull, "2026-07-04 12:00:00"},
        {2147483646ull, "2038-01-19 03:14:06"},
    };
    for (const auto& c : cases) {
        INFO(c.what);
        g_now = c.when;
        REQUIRE(r.put("/t.txt", pattern(5, 1)));
        fs::FileInfo info;
        REQUIRE(r.fs->stat("/t.txt", info));
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(info.modifiedTime.time_since_epoch()).count();
        CHECK(secs == static_cast<long long>(c.when)); // all on even seconds: FAT keeps 2 s resolution
    }
    g_now = kNow;
}

TEST_CASE("a full card fails writes cleanly and keeps what was written") {
    fakesd::Options o; o.sectors = 4096; // 2 MiB
    Rig r(o);
    const auto chunk = pattern(64 * 1024, 5);
    auto f = r.fs->open("/fill.bin", "wb");
    REQUIRE(f);
    std::size_t total = 0, wrote;
    while ((wrote = f->write(chunk.data(), chunk.size())) == chunk.size()) total += wrote;
    CHECK(total > 1024 * 1024);
    CHECK(total < 2 * 1024 * 1024);
    f.reset();
    REQUIRE(r.fs->exists("/fill.bin"));
    REQUIRE(r.fs->removeFile("/fill.bin"));
    CHECK(r.put("/after.bin", pattern(1000, 1))); // space is back
}

TEST_CASE("a card that is not FAT does not mount") {
    fakesd::Card card;
    SdCard sd(card, kRun);
    REQUIRE(sd.init());
    CHECK_FALSE(mount_sd_filesystem(sd, &clock_fn)); // all zeros
    for (std::size_t i = 0; i < 1024; ++i) card.image()[i] = static_cast<std::uint8_t>(i * 13);
    CHECK_FALSE(mount_sd_filesystem(sd, &clock_fn));
}

TEST_CASE("an absent card does not mount") {
    fakesd::Options o; o.absent = true;
    fakesd::Card card(o);
    SdCard sd(card, kRun);
    CHECK_FALSE(mount_sd_filesystem(sd, &clock_fn));
}

TEST_CASE("a card that errors mid-use fails operations, then recovers on its own once it answers again") {
    Rig r;
    REQUIRE(r.put("/keep.bin", pattern(2000, 1)));
    auto open_file = r.fs->open("/keep.bin", "rb");
    REQUIRE(open_file);

    r.card.options().absent = true;      // card pulled
    std::uint8_t b[16];
    CHECK_FALSE(r.put("/new.bin", pattern(10, 1)));   // needs the card: fails, the volume is dropped
    CHECK_FALSE(r.fs->exists("/keep.bin"));           // and stays unavailable while the card is away
    r.card.delay_ms(1500);               // the retry interval passes
    CHECK_FALSE(r.fs->exists("/keep.bin"));

    r.card.options().absent = false;     // card back
    r.card.delay_ms(1500);
    CHECK(r.fs->exists("/keep.bin"));
    CHECK(r.get("/keep.bin") == pattern(2000, 1));  // data intact
    CHECK(open_file->read(b, 16) == 0);  // a handle from before the glitch is dead, not corrupt
    CHECK(r.put("/new.bin", pattern(10, 1)));
}

TEST_CASE("available() says the card is gone as soon as it is pulled, before any access has failed") {
    Rig r;
    REQUIRE(r.put("/keep.bin", pattern(2000, 1)));
    CHECK(r.fs->available());
    r.card.options().absent = true;          // pulled while nothing is using it
    CHECK_FALSE(r.fs->available());          // no read or write has failed yet
    CHECK_FALSE(r.fs->exists("/keep.bin"));
    CHECK_FALSE(r.put("/new.bin", pattern(10, 1))); // behaviour is unchanged: the write fails

    r.card.options().absent = false;         // back in
    r.card.delay_ms(1500);                   // the retry interval passes
    CHECK(r.fs->available());                // recovered without being told
    CHECK(r.get("/keep.bin") == pattern(2000, 1));
}

TEST_CASE("available() does not mount a card that was never there and answers promptly while it is away") {
    Rig r;
    r.card.options().absent = true;
    CHECK_FALSE(r.fs->available());
    const auto t = r.card.now_ms();
    for (int i = 0; i < 20; ++i) CHECK_FALSE(r.fs->available());
    CHECK(r.card.now_ms() - t < 50);         // re-initialisation is rate limited like any other access
}

TEST_CASE("available() has no side effects on the data") {
    Rig r;
    REQUIRE(r.put("/a.bin", pattern(3000, 7)));
    const auto before = r.card.writes;
    for (int i = 0; i < 10; ++i) CHECK(r.fs->available());
    CHECK(r.card.writes == before);
    CHECK(r.get("/a.bin") == pattern(3000, 7));
}

TEST_CASE("while the card is away each call returns promptly instead of re-initialising every time") {
    Rig r;
    r.card.options().absent = true;
    CHECK_FALSE(r.put("/x", pattern(10, 1)));  // the failed write drops the volume
    CHECK_FALSE(r.fs->exists("/x"));     // this call pays for the failed re-initialisation
    const auto t = r.card.now_ms();
    for (int i = 0; i < 20; ++i) CHECK_FALSE(r.fs->exists("/x"));
    CHECK(r.card.now_ms() - t < 50);
}

TEST_CASE("the filesystem FatFs writes is a valid FAT volume (checked with fsck.vfat when present)") {
    Rig r;
    REQUIRE(r.fs->createDirectory("/dir"));
    REQUIRE(r.put("/dir/one.bin", pattern(70000, 1)));
    REQUIRE(r.put("/Two Long Name.bin", pattern(9000, 2)));
    r.fs.reset(); // unmounts: everything flushed to the image
    const std::string path = "fake-sd-image.img";
    { std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(r.card.image().data()), r.card.image().size()); }
    if (std::system("command -v fsck.vfat >/dev/null 2>&1") != 0) { MESSAGE("fsck.vfat not installed: skipped"); return; }
    // The image has an MBR partition table: let fsck see the partition by offset.
    const auto& img = r.card.image();
    const std::uint32_t start = img[0x1C6] | img[0x1C7] << 8 | img[0x1C8] << 16 | static_cast<std::uint32_t>(img[0x1C9]) << 24;
    const std::uint32_t count = img[0x1CA] | img[0x1CB] << 8 | img[0x1CC] << 16 | static_cast<std::uint32_t>(img[0x1CD]) << 24;
    REQUIRE(start > 0); REQUIRE(count > 0);
    const std::string part = "fake-sd-part.img";
    REQUIRE(std::system(("dd if=" + path + " of=" + part + " bs=512 skip=" + std::to_string(start) + " count=" + std::to_string(count) + " status=none").c_str()) == 0);
    CHECK(std::system(("fsck.vfat -n " + part + " >fsck.out 2>&1").c_str()) == 0);
}
