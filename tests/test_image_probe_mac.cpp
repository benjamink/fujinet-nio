#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/image_probers/image_probe.h"
#include "fujinet/disk/image_registry.h"
#include "fujinet/fs/storage_manager.h"

#include <cstdint>
#include <memory>
#include <vector>

using namespace fujinet::disk;

namespace {

ImageProbeResult probe(std::vector<std::uint8_t>& bytes, const char* path)
{
    fujinet::tests::MemoryFile file(bytes, true, nullptr, nullptr);
    return probe_image(file, bytes.size(), path, MountOptions{});
}

} // namespace

TEST_CASE("Mac volume images probe as raw 512-byte blocks")
{
    std::vector<std::uint8_t> bytes(20480 * 512, 0);
    for (const char* path : {"/boot.hda", "/vol.HFV"}) {
        const auto r = probe(bytes, path);
        CHECK(r.matched);
        CHECK(r.type == ImageType::Raw);
        CHECK(r.geometry.sectorSize == 512);
        CHECK(r.geometry.sectorCount == 20480);
    }
}

TEST_CASE("A Mac volume extension on a size that is not whole blocks is not matched")
{
    std::vector<std::uint8_t> bytes(1000, 0);
    CHECK(!probe(bytes, "/odd.hda").matched);
}

TEST_CASE("DiskService auto-detects a Mac volume and reads its 512-byte blocks")
{
    fujinet::fs::StorageManager storage;
    auto owned = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto& bytes = owned->file_bytes("/system.hda");
    bytes.assign(40 * 512, 0);
    bytes[7 * 512] = 0x42;
    REQUIRE(storage.registerFileSystem(std::move(owned)));

    DiskService svc(storage, make_default_image_registry());
    REQUIRE(svc.mount(0, "mem", "/system.hda", {}).ok());
    const auto info = svc.info(0);
    CHECK(info.type == ImageType::Raw);
    CHECK(info.geometry.sectorSize == 512);
    CHECK(info.geometry.sectorCount == 40);

    std::uint8_t block[512]{};
    REQUIRE(svc.read_sector(0, 7, block, sizeof(block)).ok());
    CHECK(block[0] == 0x42);
}
