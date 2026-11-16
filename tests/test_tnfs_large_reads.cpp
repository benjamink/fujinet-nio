#include "doctest.h"

#include "fake_tnfs_server.h"

#include "fujinet/fs/tnfs_filesystem.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/tnfs/tnfs_protocol.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

using fujinet::tests::FakeTnfsServer;
using namespace fujinet::tnfs;

namespace {

std::vector<std::uint8_t> pattern(std::size_t n)
{
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xFF);
    return v;
}

// A TNFS read returns at most one 512-byte block, so a larger request is filled by
// several reads. Before this, it returned 512 bytes and FileDevice took that short
// result for the end of the file.
void check_large_reads(bool parallelReads)
{
    auto server = std::make_shared<FakeTnfsServer>(parallelReads);
    const auto data = pattern(3000);
    server->files["/big.bin"] = data;
    auto fs = fujinet::fs::make_tnfs_filesystem(server);
    REQUIRE(fs);

    auto file = fs->open("/big.bin", "rb");
    REQUIRE(file);

    std::vector<std::uint8_t> out(4096);
    // 1024 is two blocks: both must arrive, not just the first.
    CHECK(file->read(out.data(), 1024) == 1024);
    CHECK(std::memcmp(out.data(), data.data(), 1024) == 0);
    CHECK(file->read(out.data(), 1024) == 1024);
    CHECK(std::memcmp(out.data(), data.data() + 1024, 1024) == 0);
    // Only the last read is short, because the file ends there.
    CHECK(file->read(out.data(), 1024) == 3000 - 2048);
    CHECK(std::memcmp(out.data(), data.data() + 2048, 3000 - 2048) == 0);
    CHECK(file->read(out.data(), 1024) == 0);

    // Larger than the whole file, and not a multiple of the block size.
    REQUIRE(file->seek(0));
    CHECK(file->read(out.data(), 4096) == 3000);
    CHECK(std::memcmp(out.data(), data.data(), 3000) == 0);
    REQUIRE(file->seek(100));
    CHECK(file->read(out.data(), 1500) == 1500);
    CHECK(std::memcmp(out.data(), data.data() + 100, 1500) == 0);
    CHECK(file->tell() == 1600);
}

} // namespace

TEST_CASE("TNFS reads larger than one block are filled, not cut at 512 bytes")
{
    SUBCASE("serial client") { check_large_reads(false); }
    SUBCASE("parallel client") { check_large_reads(true); }
}

TEST_CASE("TNFS reads of exactly one block or less are unchanged")
{
    auto server = std::make_shared<FakeTnfsServer>(false);
    const auto data = pattern(1300);
    server->files["/f.bin"] = data;
    auto fs = fujinet::fs::make_tnfs_filesystem(server);
    auto file = fs->open("/f.bin", "rb");
    REQUIRE(file);

    std::vector<std::uint8_t> out(1024);
    CHECK(file->read(out.data(), 512) == 512);
    CHECK(file->read(out.data(), 100) == 100);
    CHECK(std::memcmp(out.data(), data.data() + 512, 100) == 0);
    REQUIRE(file->seek(1200));
    CHECK(file->read(out.data(), 512) == 100); // the end of the file
}
