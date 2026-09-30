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

// Image detection policy (docs/disk_device_protocol.md, "Adding a new image
// format"), one row per real-world variant:
//
//  1. Content probes win, whatever the extension.
//  2. Only extensions that mean the same thing on every machine may imply
//     geometry (.atr, .ssd, .adf, .hda, .hfv).
//  3. Ambiguous extensions never guess: .img/.ima/.raw need content or a client
//     sector-size hint, and .dsk (Apple II, Mac, CPC, MSX, TRS-80) is not
//     claimed by extension at all.
//
// A new probe or extension must keep every row passing, or change the row
// deliberately.

using namespace fujinet::disk;

namespace {

using Bytes = std::vector<std::uint8_t>;

void put_le16(Bytes& v, std::size_t at, std::uint16_t x)
{
    v[at] = static_cast<std::uint8_t>(x);
    v[at + 1] = static_cast<std::uint8_t>(x >> 8);
}

void put_be32(Bytes& v, std::size_t at, std::uint32_t x)
{
    v[at] = static_cast<std::uint8_t>(x >> 24);
    v[at + 1] = static_cast<std::uint8_t>(x >> 16);
    v[at + 2] = static_cast<std::uint8_t>(x >> 8);
    v[at + 3] = static_cast<std::uint8_t>(x);
}

Bytes zeros(std::size_t n) { return Bytes(n, 0); }

// Atari ATR: 16-byte header, then 720 single-density 128-byte sectors.
Bytes atr_sd()
{
    Bytes v(16 + 720 * 128, 0);
    put_le16(v, 0, 0x0296);
    put_le16(v, 2, static_cast<std::uint16_t>((720 * 128) / 16));
    put_le16(v, 4, 128);
    return v;
}

// Apple DiskCopy 4.2: 84-byte header, then 512-byte sectors.
Bytes dc42(std::uint32_t sectors, std::uint8_t diskFormat)
{
    Bytes v(0x54 + sectors * 512, 0);
    v[0] = 4;
    std::memcpy(&v[1], "Disk", 4);
    put_be32(v, 0x40, sectors * 512);
    v[0x50] = diskFormat;
    v[0x51] = 0x22;
    v[0x52] = 0x01;
    v[0x53] = 0x00;
    return v;
}

// PC 1.44 MB FAT12 floppy boot sector.
Bytes fat_1440k()
{
    Bytes v(2880 * 512, 0);
    v[0] = 0xEB; v[1] = 0x3C; v[2] = 0x90;
    put_le16(v, 11, 512);
    v[13] = 1;
    put_le16(v, 14, 1);
    v[16] = 2;
    put_le16(v, 17, 224);
    put_le16(v, 19, 2880);
    put_le16(v, 22, 9);
    v[510] = 0x55; v[511] = 0xAA;
    return v;
}

// Amstrad CPC extended .dsk: its own header, not a flat block image.
Bytes cpc_extended(std::size_t size)
{
    Bytes v(size, 0);
    const char sig[] = "EXTENDED CPC DSK File\r\nDisk-Info\r\n";
    std::memcpy(v.data(), sig, sizeof(sig) - 1);
    return v;
}

struct Row {
    const char* variant;
    Bytes bytes;
    const char* path;
    std::uint16_t hint;
    bool matched;
    ImageType type;
    std::uint16_t sectorSize;  // 0: probe gives no geometry
    std::uint32_t sectorCount;
};

} // namespace

TEST_CASE("Image probe matrix: each known variant gets exactly the policy outcome")
{
    const std::vector<Row> rows = {
        // Content probes, whatever the extension.
        {"ATR header", atr_sd(), "/game.atr", 0, true, ImageType::Atr, 128, 720},
        {"DiskCopy 4.2 800K named .dsk", dc42(1600, 1), "/mac.dsk", 0, true, ImageType::DiskCopy42, 512, 1600},
        {"DiskCopy 4.2 800K named .image", dc42(1600, 1), "/mac.image", 0, true, ImageType::DiskCopy42, 512, 1600},
        {"FAT 1.44M floppy .img", fat_1440k(), "/dos.img", 0, true, ImageType::Raw, 512, 2880},

        // Unambiguous extensions imply geometry.
        {"Amiga DD .adf", zeros(1760 * 512), "/wb.adf", 0, true, ImageType::Raw, 512, 1760},
        {"BBC DFS .ssd", zeros(800 * 256), "/bbc.ssd", 0, true, ImageType::Ssd, 0, 0},
        {"Mac HD .hda", zeros(2048 * 512), "/hd20.hda", 0, true, ImageType::Raw, 512, 2048},
        {"Mac HD .HFV (case-insensitive)", zeros(2048 * 512), "/vol.HFV", 0, true, ImageType::Raw, 512, 2048},

        // Ambiguous extensions never guess.
        {"headerless .img, no hint", zeros(1440 * 512), "/blank.img", 0, true, ImageType::Raw, 0, 0},
        {"headerless .img, client hint 512", zeros(1440 * 512), "/blank.img", 512, true, ImageType::Raw, 512, 1440},
        {"Apple II 140K .dsk, no hint", zeros(143360), "/dos33.dsk", 0, false, ImageType::Auto, 0, 0},
        {"Apple II 140K .dsk, client hint 256", zeros(143360), "/dos33.dsk", 256, true, ImageType::Raw, 256, 560},
        {"Mac volume named .dsk, no hint", zeros(2048 * 512), "/hd.dsk", 0, false, ImageType::Auto, 0, 0},
        {"Mac volume named .dsk, client hint 512", zeros(2048 * 512), "/hd.dsk", 512, true, ImageType::Raw, 512, 2048},
        {"CPC extended .dsk", cpc_extended(194816), "/cpc.dsk", 0, false, ImageType::Auto, 0, 0},
        {"CPC extended .dsk, 512-aligned size", cpc_extended(390 * 512), "/cpc.dsk", 0, false, ImageType::Auto, 0, 0},

        // Near misses are rejected, not reinterpreted.
        {"DiskCopy-like header, unknown disk format", dc42(1600, 7), "/bad.dsk", 0, false, ImageType::Auto, 0, 0},
        {"Mac .hda not whole blocks", zeros(1000), "/odd.hda", 0, false, ImageType::Auto, 0, 0},
    };

    for (const auto& row : rows) {
        const std::string variant = row.variant;
        CAPTURE(variant);
        Bytes bytes = row.bytes;
        fujinet::tests::MemoryFile file(bytes, true, nullptr, nullptr);
        MountOptions opts{};
        opts.sectorSizeHint = row.hint;
        const auto r = probe_image(file, bytes.size(), row.path, opts);
        CHECK(r.matched == row.matched);
        if (!row.matched) continue;
        CHECK(r.type == row.type);
        CHECK(r.geometry.sectorSize == row.sectorSize);
        CHECK(r.geometry.sectorCount == row.sectorCount);
    }
}

TEST_CASE("Image probe matrix: an Apple II .dsk keeps today's mount behaviour")
{
    fujinet::fs::StorageManager storage;
    auto owned = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    owned->file_bytes("/dos33.dsk") = zeros(143360);
    REQUIRE(storage.registerFileSystem(std::move(owned)));
    DiskService svc(storage, make_default_image_registry());

    // Auto-detection declines to guess, so the client must say what it is.
    CHECK(svc.mount(0, "mem", "/dos33.dsk", {}).error == DiskError::UnsupportedImageType);

    // An explicit raw mount keeps the raw handler's 256-byte sectors.
    MountOptions raw{};
    raw.typeOverride = ImageType::Raw;
    REQUIRE(svc.mount(1, "mem", "/dos33.dsk", raw).ok());
    CHECK(svc.info(1).geometry.sectorSize == 256);
    CHECK(svc.info(1).geometry.sectorCount == 560);
}
