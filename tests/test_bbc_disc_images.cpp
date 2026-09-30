#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/image_probers/image_probe.h"
#include "fujinet/disk/image_registry.h"
#include "fujinet/fs/storage_manager.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// BBC disc images: double-sided DFS (.dsd) and old-map ADFS (S/M/L).
// Layouts follow the b2 emulator's disc geometry.

using namespace fujinet::disk;

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::size_t kSector = 256;
constexpr std::size_t kSectorsPerTrack = 10;

std::size_t dsd_offset(std::size_t side, std::size_t track, std::size_t sector)
{
    return ((track * 2 + side) * kSectorsPerTrack + sector) * kSector;
}

void write_catalogue_size(Bytes& v, std::size_t sector1Offset, std::uint32_t sectors)
{
    v[sector1Offset + 6] = static_cast<std::uint8_t>((sectors >> 8) & 0x03);
    v[sector1Offset + 7] = static_cast<std::uint8_t>(sectors & 0xFF);
}

// A DSD whose every sector starts with {side, track, sector, 0xA5}, truncated
// to `fileBytes` (the full size when 0). Catalogues say `tracks` per side.
Bytes make_dsd(std::size_t tracks, std::size_t fileBytes = 0)
{
    const std::size_t full = tracks * 2 * kSectorsPerTrack * kSector;
    Bytes v(full, 0);
    for (std::size_t t = 0; t < tracks; ++t) {
        for (std::size_t s = 0; s < 2; ++s) {
            for (std::size_t k = 0; k < kSectorsPerTrack; ++k) {
                const std::size_t off = dsd_offset(s, t, k);
                v[off] = static_cast<std::uint8_t>(s);
                v[off + 1] = static_cast<std::uint8_t>(t);
                v[off + 2] = static_cast<std::uint8_t>(k);
                v[off + 3] = 0xA5;
            }
        }
    }
    const auto sectorsPerSide = static_cast<std::uint32_t>(tracks * kSectorsPerTrack);
    write_catalogue_size(v, dsd_offset(0, 0, 1), sectorsPerSide);
    write_catalogue_size(v, dsd_offset(1, 0, 1), sectorsPerSide);
    if (fileBytes != 0) v.resize(fileBytes);
    return v;
}

// An old-map ADFS disc of `sectors` 256-byte sectors: disc size in the free
// space map and the root directory's "Hugo" start and end markers.
Bytes make_adfs(std::uint32_t sectors)
{
    Bytes v(static_cast<std::size_t>(sectors) * kSector, 0);
    v[0xFC] = static_cast<std::uint8_t>(sectors);
    v[0xFD] = static_cast<std::uint8_t>(sectors >> 8);
    v[0xFE] = static_cast<std::uint8_t>(sectors >> 16);
    std::memcpy(&v[0x201], "Hugo", 4);
    std::memcpy(&v[0x6FB], "Hugo", 4);
    v[7 * kSector] = 0x77;
    return v;
}

struct Fixture {
    fujinet::fs::StorageManager storage;
    fujinet::tests::MemoryFileSystem* mem{nullptr};
    std::unique_ptr<DiskService> svc;

    Fixture()
    {
        auto owned = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
        mem = owned.get();
        REQUIRE(storage.registerFileSystem(std::move(owned)));
        svc = std::make_unique<DiskService>(storage, make_default_image_registry());
    }
};

// Reads logical sector `lba` of slot 0 and returns its first three bytes as
// {side, track, sector}.
std::vector<std::uint8_t> marker(DiskService& svc, std::uint32_t lba)
{
    std::uint8_t sector[kSector]{};
    REQUIRE(svc.read_sector(0, lba, sector, sizeof(sector)).ok());
    return {sector[0], sector[1], sector[2]};
}

} // namespace

TEST_CASE("DSD: an 80-track image serves side 0 then side 1 from interleaved tracks")
{
    Fixture f;
    f.mem->file_bytes("/disc.dsd") = make_dsd(80);
    REQUIRE(f.svc->mount(0, "mem", "/disc.dsd", {}).ok());
    const auto info = f.svc->info(0);
    CHECK(info.type == ImageType::Dsd);
    CHECK(info.geometry.sectorSize == 256);
    CHECK(info.geometry.sectorCount == 1600);

    // Side 0 (DFS drive 0): logical sectors 0-799.
    CHECK(marker(*f.svc, 0) == std::vector<std::uint8_t>{0, 0, 0});
    CHECK(marker(*f.svc, 15) == std::vector<std::uint8_t>{0, 1, 5});
    CHECK(marker(*f.svc, 799) == std::vector<std::uint8_t>{0, 79, 9});
    // Side 1 (DFS drive 2): logical sectors 800-1599.
    CHECK(marker(*f.svc, 800) == std::vector<std::uint8_t>{1, 0, 0});
    CHECK(marker(*f.svc, 812) == std::vector<std::uint8_t>{1, 1, 2});
    CHECK(marker(*f.svc, 1599) == std::vector<std::uint8_t>{1, 79, 9});
}

TEST_CASE("DSD: a 40-track image has 400 sectors a side")
{
    Fixture f;
    f.mem->file_bytes("/disc40.dsd") = make_dsd(40);
    REQUIRE(f.svc->mount(0, "mem", "/disc40.dsd", {}).ok());
    CHECK(f.svc->info(0).geometry.sectorCount == 800);
    CHECK(marker(*f.svc, 399) == std::vector<std::uint8_t>{0, 39, 9});
    CHECK(marker(*f.svc, 400) == std::vector<std::uint8_t>{1, 0, 0});
    CHECK(marker(*f.svc, 799) == std::vector<std::uint8_t>{1, 39, 9});
}

TEST_CASE("DSD: a truncated image reads blank past its end and grows on write")
{
    Fixture f;
    // Real images are often short: this one ends 10 sectors before a full
    // 80-track DSD (as a real 407,040-byte image does).
    const std::size_t truncated = 1590 * kSector;
    f.mem->file_bytes("/short.dsd") = make_dsd(80, truncated);
    MountOptions rw{};
    REQUIRE(f.svc->mount(0, "mem", "/short.dsd", rw).ok());
    CHECK(f.svc->info(0).geometry.sectorCount == 1600);

    // Side 1, track 79 is past the end of the file.
    std::uint8_t sector[kSector];
    std::memset(sector, 0xFF, sizeof(sector));
    REQUIRE(f.svc->read_sector(0, 1599, sector, sizeof(sector)).ok());
    CHECK(sector[0] == 0);
    CHECK(sector[255] == 0);

    std::vector<std::uint8_t> data(kSector, 0x5C);
    REQUIRE(f.svc->write_sector(0, 1599, data.data(), data.size()).ok());
    REQUIRE(f.svc->flush(0).ok());
    const auto& bytes = f.mem->file_bytes("/short.dsd");
    const std::size_t off = dsd_offset(1, 79, 9);
    REQUIRE(bytes.size() == off + kSector);
    CHECK(bytes[off] == 0x5C);
    CHECK(bytes[off + 255] == 0x5C);
    REQUIRE(f.svc->read_sector(0, 1599, sector, sizeof(sector)).ok());
    CHECK(sector[0] == 0x5C);
}

TEST_CASE("DSD: a side-1 write lands in its interleaved track")
{
    Fixture f;
    f.mem->file_bytes("/disc.dsd") = make_dsd(80);
    REQUIRE(f.svc->mount(0, "mem", "/disc.dsd", {}).ok());
    std::vector<std::uint8_t> data(kSector, 0x3E);
    REQUIRE(f.svc->write_sector(0, 800 + 23, data.data(), data.size()).ok()); // side 1, track 2, sector 3
    REQUIRE(f.svc->flush(0).ok());
    const auto& bytes = f.mem->file_bytes("/disc.dsd");
    CHECK(bytes[dsd_offset(1, 2, 3)] == 0x3E);
    CHECK(bytes[dsd_offset(0, 2, 3)] == 0);  // side 0's sector untouched
    CHECK(bytes[dsd_offset(0, 2, 3) + 1] == 2);
}

TEST_CASE("DSD: an image without a DFS catalogue is rejected")
{
    Fixture f;
    f.mem->file_bytes("/blank.dsd") = Bytes(1600 * kSector, 0);
    CHECK(f.svc->mount(0, "mem", "/blank.dsd", {}).error == DiskError::BadImage);
}

TEST_CASE("DSD: Create writes a catalogue on both sides")
{
    Fixture f;
    REQUIRE(f.svc->create_image("mem", "/new.dsd", ImageType::Dsd, 256, 1600, false).ok());
    const auto& bytes = f.mem->file_bytes("/new.dsd");
    REQUIRE(bytes.size() == 1600 * kSector);
    const std::size_t side0 = dsd_offset(0, 0, 1);
    const std::size_t side1 = dsd_offset(1, 0, 1);
    CHECK((((bytes[side0 + 6] & 0x03) << 8) | bytes[side0 + 7]) == 800);
    CHECK((((bytes[side1 + 6] & 0x03) << 8) | bytes[side1 + 7]) == 800);
    REQUIRE(f.svc->mount(0, "mem", "/new.dsd", {}).ok());
    CHECK(f.svc->info(0).geometry.sectorCount == 1600);

    CHECK(f.svc->create_image("mem", "/bad.dsd", ImageType::Dsd, 256, 1000, false).error ==
          DiskError::InvalidGeometry);
}

TEST_CASE("ADFS: old-map S, M and L discs are recognised by content, whatever the name")
{
    struct Case { const char* path; std::uint32_t sectors; };
    const Case cases[] = {
        {"/welcome.ads", 640},   // S: 40 tracks, 1 side
        {"/games.adm", 1280},    // M: 80 tracks, 1 side
        {"/master.adl", 2560},   // L: 80 tracks, 2 sides
        {"/acorn.adf", 2560},    // .adf is also Acorn's; content decides
        {"/hard.dat", 10240},    // an old-map hard disc image
    };
    for (const auto& c : cases) {
        const std::string path = c.path;
        CAPTURE(path);
        Fixture f;
        f.mem->file_bytes(c.path) = make_adfs(c.sectors);
        REQUIRE(f.svc->mount(0, "mem", c.path, {}).ok());
        const auto info = f.svc->info(0);
        CHECK(info.type == ImageType::Raw);
        CHECK(info.geometry.sectorSize == 256);
        CHECK(info.geometry.sectorCount == c.sectors);
        std::uint8_t sector[kSector]{};
        REQUIRE(f.svc->read_sector(0, 7, sector, sizeof(sector)).ok());
        CHECK(sector[0] == 0x77);
    }
}

TEST_CASE("ADFS: near misses are not taken for ADFS")
{
    auto probe = [](Bytes bytes, const char* path) {
        fujinet::tests::MemoryFile file(bytes, true, nullptr, nullptr);
        return probe_image(file, bytes.size(), path, MountOptions{});
    };
    // Disc size in the map disagrees with the file.
    Bytes wrongSize = make_adfs(640);
    wrongSize.resize(700 * kSector, 0);
    CHECK_FALSE(probe(wrongSize, "/wrong.adl").matched);
    // Missing the directory's end marker.
    Bytes noEnd = make_adfs(640);
    std::memset(&noEnd[0x6FB], 0, 4);
    CHECK_FALSE(probe(noEnd, "/noend.adl").matched);
    // An Amiga ADF is still an Amiga ADF.
    const auto amiga = probe(Bytes(1760 * 512, 0), "/workbench.adf");
    CHECK(amiga.matched);
    CHECK(amiga.geometry.sectorSize == 512);
    CHECK(amiga.geometry.sectorCount == 1760);
}
