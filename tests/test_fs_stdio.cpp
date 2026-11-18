#include "doctest.h"

#include "fujinet/fs/fs_stdio.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace {

std::vector<std::uint8_t> read_back(const std::filesystem::path& p)
{
    std::vector<std::uint8_t> out(std::filesystem::file_size(p));
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    REQUIRE(f);
    REQUIRE(std::fread(out.data(), 1, out.size(), f) == out.size());
    std::fclose(f);
    return out;
}

} // namespace

TEST_CASE("StdioFile: a write right after a read lands at the logical position")
{
    const auto dir = std::filesystem::temp_directory_path() / "fnio_fs_stdio_test";
    std::filesystem::create_directories(dir);
    {
        std::FILE* f = std::fopen((dir / "img.bin").string().c_str(), "wb");
        REQUIRE(f);
        std::vector<std::uint8_t> zeros(16 * 512, 0);
        std::fwrite(zeros.data(), 1, zeros.size(), f);
        std::fclose(f);
    }

    auto fs = fujinet::fs::create_stdio_filesystem(dir.string(), "t", fujinet::fs::FileSystemKind::HostPosix);
    REQUIRE(fs);
    {
        auto file = fs->open("/img.bin", "r+b");
        REQUIRE(file);
        std::uint8_t block[512];
        REQUIRE(file->seek(0));
        REQUIRE(file->read(block, sizeof(block)) == sizeof(block)); // stdio buffers ahead
        std::vector<std::uint8_t> ones(512, 0x11);
        REQUIRE(file->write(ones.data(), ones.size()) == ones.size()); // block 1
        REQUIRE(file->read(block, sizeof(block)) == sizeof(block));    // block 2
        CHECK(block[0] == 0);
        std::vector<std::uint8_t> twos(512, 0x22);
        REQUIRE(file->write(twos.data(), twos.size()) == twos.size()); // block 3
        REQUIRE(file->flush());
    }

    const auto bytes = read_back(dir / "img.bin");
    CHECK(bytes[0 * 512] == 0);
    CHECK(bytes[1 * 512] == 0x11);
    CHECK(bytes[2 * 512] == 0);
    CHECK(bytes[3 * 512] == 0x22);
    CHECK(bytes[8 * 512] == 0);
    std::filesystem::remove_all(dir);
}

namespace {
bool g_medium_in = true;
int g_probes = 0;
bool medium_in() noexcept { return g_medium_in; }
bool counted_medium_in() noexcept { ++g_probes; return g_medium_in; }
} // namespace

TEST_CASE("stdio filesystem: available() is true by default and follows an injected probe")
{
    const auto dir = std::filesystem::temp_directory_path() / "fnio_fs_stdio_available";
    std::filesystem::create_directories(dir);

    // No probe: nothing can take the medium away.
    auto plain = fujinet::fs::create_stdio_filesystem(dir.string(), "plain", fujinet::fs::FileSystemKind::HostPosix);
    REQUIRE(plain);
    CHECK(plain->available());

    auto card = fujinet::fs::create_stdio_filesystem(dir.string(), "sd0", fujinet::fs::FileSystemKind::LocalSD,
                                                     nullptr, &medium_in);
    REQUIRE(card);
    g_medium_in = true;
    CHECK(card->available());
    g_medium_in = false;                       // card pulled
    CHECK_FALSE(card->available());
    CHECK(plain->available());                 // only the filesystem with the probe is affected
    g_medium_in = true;                        // card back
    CHECK(card->available());
    std::filesystem::remove_all(dir);
}

TEST_CASE("stdio filesystem: operations give a removed medium the chance to recover")
{
    // The ESP32 remounts a card that is back from inside its probe. A file operation must reach
    // the probe, or an application writing to a re-inserted card would fail until something else
    // asked available().
    const auto dir = std::filesystem::temp_directory_path() / "fnio_fs_stdio_recover";
    std::filesystem::create_directories(dir);
    auto card = fujinet::fs::create_stdio_filesystem(dir.string(), "sd0", fujinet::fs::FileSystemKind::LocalSD,
                                                     nullptr, &counted_medium_in);
    REQUIRE(card);

    SUBCASE("while the medium is down every operation probes it") {
        g_medium_in = false;
        CHECK_FALSE(card->available());
        g_probes = 0;
        card->exists("/a");
        card->exists("/b");
        card->exists("/c");
        CHECK(g_probes == 3);
    }
    SUBCASE("an operation after the medium is back probes it again") {
        g_medium_in = false;
        CHECK_FALSE(card->available());
        g_medium_in = true;                    // back, but nobody has asked available()
        g_probes = 0;
        card->exists("/a");
        CHECK(g_probes == 1);
        card->exists("/b");                    // up again: throttled, no probe per operation
        CHECK(g_probes == 1);
    }
    SUBCASE("a burst of operations on a medium that is up probes it only a few times") {
        g_medium_in = true;
        g_probes = 0;
        for (int i = 0; i < 100; ++i) card->exists("/x");
        CHECK(g_probes >= 1);
        CHECK(g_probes <= 3);
    }
    std::filesystem::remove_all(dir);
}
