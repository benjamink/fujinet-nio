#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/disk/image_probers/image_probe.h"

#include <cstdint>
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
    for (const char* path : {"/boot.hda", "/vol.HFV", "/disk.dsk"}) {
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
