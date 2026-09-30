#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/disk/dc42_image.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/image_probers/image_probe.h"
#include "fujinet/disk/image_registry.h"
#include "fujinet/fs/storage_manager.h"

#include <cstdint>
#include <memory>
#include <vector>

using namespace fujinet::disk;
using fujinet::tests::MemoryFile;

namespace {

constexpr std::size_t kHeader = 0x54;

void put_be32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x)
{
    v[at] = x >> 24;
    v[at + 1] = x >> 16;
    v[at + 2] = x >> 8;
    v[at + 3] = x;
}

std::vector<std::uint8_t> make_dc42(std::uint32_t sectors, std::uint32_t tagBytes = 0)
{
    std::vector<std::uint8_t> v(kHeader + sectors * 512 + tagBytes);
    const char name[] = "Test Disk";
    v[0] = sizeof(name) - 1;
    for (std::size_t i = 0; i + 1 < sizeof(name); ++i) v[1 + i] = name[i];
    put_be32(v, 0x40, sectors * 512);
    put_be32(v, 0x44, tagBytes);
    v[0x50] = 1;    // 800K
    v[0x51] = 0x22; // Mac format
    v[0x52] = 0x01;
    v[0x53] = 0x00;
    for (std::uint32_t s = 0; s < sectors; ++s) v[kHeader + s * 512] = static_cast<std::uint8_t>(s);
    put_be32(v, 0x48, dc42_checksum_update(0, v.data() + kHeader, sectors * 512));
    return v;
}

std::uint32_t get_be32(const std::vector<std::uint8_t>& v, std::size_t at)
{
    return (std::uint32_t(v[at]) << 24) | (std::uint32_t(v[at + 1]) << 16) |
           (std::uint32_t(v[at + 2]) << 8) | v[at + 3];
}

} // namespace

TEST_CASE("DiskCopy 4.2 images are found by their header, whatever the extension")
{
    auto bytes = make_dc42(1600, 1600 * 12);
    for (const char* path : {"/disk.image", "/disk.img", "/disk.dsk"}) {
        MemoryFile file(bytes, true, nullptr, nullptr);
        const auto r = probe_image(file, bytes.size(), path, MountOptions{});
        CHECK(r.type == ImageType::DiskCopy42);
        CHECK(r.geometry.sectorSize == 512);
        CHECK(r.geometry.sectorCount == 1600);
    }
}

TEST_CASE("DiskCopy 4.2 header checks")
{
    Dc42Header h;
    auto good = make_dc42(800);
    CHECK(parse_dc42_header(good.data(), good.size(), h));
    CHECK(h.dataBytes == 800 * 512);

    auto badMagic = good;
    badMagic[0x52] = 0;
    CHECK(!parse_dc42_header(badMagic.data(), badMagic.size(), h));

    CHECK(!parse_dc42_header(good.data(), good.size() - 1, h)); // data runs past the file

    auto badName = good;
    badName[0] = 64;
    CHECK(!parse_dc42_header(badName.data(), badName.size(), h));

    auto badFormat = good;
    badFormat[0x50] = 4; // only 400K, 800K, 720K and 1440K exist
    CHECK(!parse_dc42_header(badFormat.data(), badFormat.size(), h));

    auto tagged = make_dc42(800, 800 * 12);
    CHECK(parse_dc42_header(tagged.data(), tagged.size(), h));
    CHECK(h.tagBytes == 800 * 12);

    auto oddTags = make_dc42(800, 5); // tags are 12 bytes per sector or absent
    CHECK(!parse_dc42_header(oddTags.data(), oddTags.size(), h));
}

TEST_CASE("DiskCopy 4.2 checksum adds big-endian words and rotates right")
{
    const std::uint8_t zeros[4] = {};
    CHECK(dc42_checksum_update(0, zeros, sizeof(zeros)) == 0);
    const std::uint8_t one[2] = {0x00, 0x01};
    CHECK(dc42_checksum_update(0, one, sizeof(one)) == 0x80000000u);
    const std::uint8_t two[4] = {0x00, 0x01, 0x00, 0x01};
    CHECK(dc42_checksum_update(0, two, sizeof(two)) == 0xC0000000u);
    const std::uint8_t word[2] = {0x12, 0x34}; // big-endian 0x1234
    CHECK(dc42_checksum_update(0, word, sizeof(word)) == 0x0000091Au);
    // Folding in pieces equals folding in one go.
    CHECK(dc42_checksum_update(dc42_checksum_update(0, two, 2), two + 2, 2) ==
          dc42_checksum_update(0, two, sizeof(two)));
}

TEST_CASE("DiskService auto-detects, reads and writes a DiskCopy 4.2 image, then fixes its checksum")
{
    fujinet::fs::StorageManager storage;
    auto owned = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* mem = owned.get();
    mem->file_bytes("/mac.image") = make_dc42(1600, 1600 * 12);
    const auto original = mem->file_bytes("/mac.image");
    REQUIRE(storage.registerFileSystem(std::move(owned)));

    DiskService svc(storage, make_default_image_registry());
    REQUIRE(svc.mount(0, "mem", "/mac.image", {}).ok());
    const auto info = svc.info(0);
    CHECK(info.type == ImageType::DiskCopy42);
    CHECK(info.geometry.sectorSize == 512);
    CHECK(info.geometry.sectorCount == 1600);

    std::uint8_t sector[512]{};
    REQUIRE(svc.read_sector(0, 3, sector, sizeof(sector)).ok());
    CHECK(sector[0] == 3);

    std::vector<std::uint8_t> ones(512, 0x11);
    REQUIRE(svc.write_sector(0, 10, ones.data(), ones.size()).ok());
    REQUIRE(svc.flush(0).ok());

    const auto& after = mem->file_bytes("/mac.image");
    REQUIRE(after.size() == original.size());
    const std::uint32_t expected = dc42_checksum_update(0, after.data() + kHeader, 1600 * 512);
    CHECK(get_be32(after, 0x48) == expected);
    CHECK(get_be32(after, 0x48) != get_be32(original, 0x48));
    // Only the written sector and the data checksum changed: the rest of the
    // header, the other sectors and the tags are untouched.
    for (std::size_t i = 0; i < after.size(); ++i) {
        const bool checksum = i >= 0x48 && i < 0x4C;
        const bool written = i >= kHeader + 10 * 512 && i < kHeader + 11 * 512;
        if (!checksum && !written && after[i] != original[i]) {
            CAPTURE(i);
            FAIL("unexpected byte changed");
        }
    }
}

TEST_CASE("DiskCopy 4.2 sectors are read and written past the header")
{
    auto bytes = make_dc42(800);
    auto image = make_dc42_disk_image();
    MountOptions opts{};
    opts.readOnlyRequested = false;
    REQUIRE(image->mount(std::make_unique<MemoryFile>(bytes, false, nullptr, nullptr), bytes.size(), opts).ok());
    CHECK(image->geometry().sectorCount == 800);

    std::uint8_t sector[512]{};
    REQUIRE(image->read_sector(5, sector, sizeof(sector)).ok());
    CHECK(sector[0] == 5);

    std::vector<std::uint8_t> ones(512, 0x11);
    REQUIRE(image->write_sector(7, ones.data(), ones.size()).ok());
    CHECK(bytes[kHeader + 7 * 512] == 0x11);
    CHECK(bytes[kHeader + 8 * 512] == 8);

    CHECK(image->read_sector(800, sector, sizeof(sector)).error == DiskError::OutOfRange);
}

TEST_CASE("A read-only DiskCopy 4.2 mount refuses writes")
{
    auto bytes = make_dc42(800);
    auto image = make_dc42_disk_image();
    MountOptions opts{};
    opts.readOnlyRequested = true;
    REQUIRE(image->mount(std::make_unique<MemoryFile>(bytes, true, nullptr, nullptr), bytes.size(), opts).ok());
    std::vector<std::uint8_t> ones(512, 0x11);
    CHECK(image->write_sector(0, ones.data(), ones.size()).error == DiskError::ReadOnly);
}
