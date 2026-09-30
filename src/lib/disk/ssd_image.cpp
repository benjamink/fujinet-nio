#include "fujinet/disk/ssd_image.h"

#include <algorithm>
#include <cstring>

namespace fujinet::disk {

// DFS disc size in header: sector 1 bytes 6–7 = file offsets $106/$107 (262/263).
// High 2 bits at $106, low 8 bits at $107 → 10-bit sector count.
static constexpr std::uint64_t SSD_HEADER_SECTOR_COUNT_OFF_HI = 0x106;
static constexpr std::uint64_t SSD_HEADER_SECTOR_COUNT_OFF_LO = 0x107;
static constexpr std::uint64_t SSD_HEADER_MIN_BYTES = 0x108;

// Acorn DFS disc images: 10 sectors of 256 bytes per track.
//  - SSD: one side, sectors in order.
//  - DSD: two sides (DFS drives 0 and 2), stored track-interleaved: track 0
//    side 0, track 0 side 1, track 1 side 0, ... Logical sectors run through
//    side 0, then side 1, so side 1 starts at sectorCount / 2.
// Each side's catalogue gives its size (400 or 800 sectors: 40 or 80 tracks).
// Images are often truncated; sectors past the end of the file read as zeros
// and are created on write.
static constexpr std::uint32_t DFS_SECTORS_PER_TRACK = 10;

class SsdDiskImage final : public IDiskImage {
public:
    SsdDiskImage(ImageType type, std::uint32_t sides) : _type(type), _sides(sides) {}

    ImageType type() const noexcept override { return _type; }
    DiskGeometry geometry() const noexcept override { return _geo; }
    bool read_only() const noexcept override { return _readOnly; }

    DiskResult mount(
        std::unique_ptr<fs::IFile> file,
        std::uint64_t sizeBytes,
        const MountOptions& opts
    ) override
    {
        if (!file) return DiskResult{DiskError::OpenFailed};

        // SSD is a flat 256-byte sector image. Common sizes:
        // - 40 track:  400 sectors = 102,400 bytes
        // - 80 track:  800 sectors = 204,800 bytes
        // Sparse images may be truncated; sector count comes from DFS header (bytes 106–107).
        static constexpr std::uint16_t SECTOR_SIZE = 256;

        if (sizeBytes < SSD_HEADER_MIN_BYTES) return DiskResult{DiskError::BadImage};

        std::uint8_t header[SSD_HEADER_MIN_BYTES]{};
        if (!file->seek(0)) return DiskResult{DiskError::IoError};
        if (file->read(header, sizeof(header)) != sizeof(header)) return DiskResult{DiskError::IoError};

        const std::uint32_t sectors = (static_cast<std::uint32_t>(header[SSD_HEADER_SECTOR_COUNT_OFF_HI] & 0x03u) << 8)
            | static_cast<std::uint32_t>(header[SSD_HEADER_SECTOR_COUNT_OFF_LO]);
        if (!(sectors == 400 || sectors == 800)) return DiskResult{DiskError::BadImage};

        _file = std::move(file);
        _readOnly = opts.readOnlyRequested;
        _fileSizeBytes = sizeBytes;

        _geo.sectorSize = SECTOR_SIZE;
        _sectorsPerSide = sectors;
        _geo.sectorCount = sectors * _sides;
        _geo.supportsVariableSectorSize = false;

        return DiskResult{DiskError::None};
    }

    DiskResult unmount() override
    {
        _file.reset();
        _geo = {};
        _readOnly = true;
        _fileSizeBytes = 0;
        _sectorsPerSide = 0;
        return DiskResult{DiskError::None};
    }

    DiskResult read_sector(std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes) override
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        if (!dst) return DiskResult{DiskError::InvalidSlot};
        if (_geo.sectorSize == 0 || _geo.sectorCount == 0) return DiskResult{DiskError::BadImage};
        if (lba >= _geo.sectorCount) return DiskResult{DiskError::OutOfRange};
        if (dstBytes < _geo.sectorSize) return DiskResult{DiskError::InvalidSlot};

        const std::uint64_t off = offset_of(lba);
        const std::uint64_t end = off + _geo.sectorSize;

        if (off >= _fileSizeBytes) {
            std::memset(dst, 0, _geo.sectorSize);
            return DiskResult{DiskError::None, static_cast<std::uint16_t>(_geo.sectorSize)};
        }
        if (end <= _fileSizeBytes) {
            if (!_file->seek(off)) return DiskResult{DiskError::IoError};
            const std::size_t got = _file->read(dst, _geo.sectorSize);
            if (got != _geo.sectorSize) return DiskResult{DiskError::IoError};
            return DiskResult{DiskError::None, static_cast<std::uint16_t>(_geo.sectorSize)};
        }
        std::size_t inFile = static_cast<std::size_t>(_fileSizeBytes - off);
        if (!_file->seek(off)) return DiskResult{DiskError::IoError};
        const std::size_t got = _file->read(dst, inFile);
        if (got != inFile) return DiskResult{DiskError::IoError};
        std::memset(dst + inFile, 0, _geo.sectorSize - inFile);
        return DiskResult{DiskError::None, static_cast<std::uint16_t>(_geo.sectorSize)};
    }

    DiskResult write_sector(std::uint32_t lba, const std::uint8_t* src, std::size_t srcBytes) override
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        if (_readOnly) return DiskResult{DiskError::ReadOnly};
        if (!src) return DiskResult{DiskError::InvalidSlot};
        if (_geo.sectorSize == 0 || _geo.sectorCount == 0) return DiskResult{DiskError::BadImage};
        if (lba >= _geo.sectorCount) return DiskResult{DiskError::OutOfRange};
        if (srcBytes < _geo.sectorSize) return DiskResult{DiskError::InvalidSlot};

        const std::uint64_t off = offset_of(lba);
        const std::uint64_t end = off + _geo.sectorSize;

        if (off > _fileSizeBytes) {
            static constexpr std::size_t ZERO_BUF = 256;
            std::uint8_t zeros[ZERO_BUF]{};
            if (!_file->seek(_fileSizeBytes)) return DiskResult{DiskError::IoError};
            for (std::uint64_t pos = _fileSizeBytes; pos < off; ) {
                const std::size_t chunk = static_cast<std::size_t>(std::min(off - pos, static_cast<std::uint64_t>(ZERO_BUF)));
                if (_file->write(zeros, chunk) != chunk) return DiskResult{DiskError::IoError};
                pos += chunk;
            }
            _fileSizeBytes = off;
        }
        if (!_file->seek(off)) return DiskResult{DiskError::IoError};
        const std::size_t wrote = _file->write(src, _geo.sectorSize);
        if (wrote != _geo.sectorSize) return DiskResult{DiskError::IoError};
        if (end > _fileSizeBytes) _fileSizeBytes = end;
        return DiskResult{DiskError::None, static_cast<std::uint16_t>(_geo.sectorSize)};
    }

    DiskResult flush() override
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        return DiskResult{_file->flush() ? DiskError::None : DiskError::IoError};
    }

private:
    std::uint64_t offset_of(std::uint32_t lba) const noexcept
    {
        if (_sides == 1) return static_cast<std::uint64_t>(lba) * _geo.sectorSize;
        const std::uint32_t side = lba / _sectorsPerSide;
        const std::uint32_t inSide = lba % _sectorsPerSide;
        const std::uint32_t track = inSide / DFS_SECTORS_PER_TRACK;
        const std::uint32_t sector = inSide % DFS_SECTORS_PER_TRACK;
        return (static_cast<std::uint64_t>(track * 2 + side) * DFS_SECTORS_PER_TRACK + sector) *
               _geo.sectorSize;
    }

    ImageType _type;
    std::uint32_t _sides;
    std::uint32_t _sectorsPerSide{0};
    std::unique_ptr<fs::IFile> _file;
    DiskGeometry _geo{};
    bool _readOnly{true};
    std::uint64_t _fileSizeBytes{0};  // actual file size (for sparse read/write)
};

std::unique_ptr<IDiskImage> make_ssd_disk_image()
{
    return std::make_unique<SsdDiskImage>(ImageType::Ssd, 1);
}

std::unique_ptr<IDiskImage> make_dsd_disk_image()
{
    return std::make_unique<SsdDiskImage>(ImageType::Dsd, 2);
}

DiskResult validate_ssd_image_geometry(std::uint16_t sectorSize, std::uint32_t sectorCount)
{
    if (sectorSize != 256) return DiskResult{DiskError::InvalidGeometry};
    if (!(sectorCount == 400 || sectorCount == 800)) return DiskResult{DiskError::InvalidGeometry};
    return DiskResult{DiskError::None};
}

// Writes a blank catalogue for one side of `sectorsPerSide` sectors at the
// file offsets of its sectors 0 and 1.
static DiskResult write_blank_catalogue(fs::IFile& file, std::uint64_t sector0Offset,
                                        std::uint64_t sector1Offset,
                                        std::uint32_t sectorsPerSide);

DiskResult create_ssd_image_file(fs::IFile& file, std::uint16_t sectorSize, std::uint32_t sectorCount)
{
    DiskResult validation = validate_ssd_image_geometry(sectorSize, sectorCount);
    if (!validation.ok()) return validation;
    DiskResult catalogue = write_blank_catalogue(file, 0, 256, sectorCount);
    if (!catalogue.ok()) return catalogue;

    // Ensure final file size (sparse-extend).
    const std::uint64_t total = 256ull * sectorCount;
    if (!file.seek(total - 1)) return DiskResult{DiskError::IoError};
    const std::uint8_t z = 0;
    if (file.write(&z, 1) != 1) return DiskResult{DiskError::IoError};
    return DiskResult{DiskError::None};
}

DiskResult validate_dsd_image_geometry(std::uint16_t sectorSize, std::uint32_t sectorCount)
{
    if (sectorSize != 256) return DiskResult{DiskError::InvalidGeometry};
    if (!(sectorCount == 800 || sectorCount == 1600)) return DiskResult{DiskError::InvalidGeometry};
    return DiskResult{DiskError::None};
}

DiskResult create_dsd_image_file(fs::IFile& file, std::uint16_t sectorSize, std::uint32_t sectorCount)
{
    DiskResult validation = validate_dsd_image_geometry(sectorSize, sectorCount);
    if (!validation.ok()) return validation;
    const std::uint32_t perSide = sectorCount / 2;
    // Side 0's catalogue is in track 0's first two sectors; side 1's follows
    // side 0's track 0 in the interleaved file.
    DiskResult side0 = write_blank_catalogue(file, 0, 256, perSide);
    if (!side0.ok()) return side0;
    const std::uint64_t side1Track0 = 256ull * DFS_SECTORS_PER_TRACK;
    DiskResult side1 = write_blank_catalogue(file, side1Track0, side1Track0 + 256, perSide);
    if (!side1.ok()) return side1;

    const std::uint64_t total = 256ull * sectorCount;
    if (!file.seek(total - 1)) return DiskResult{DiskError::IoError};
    const std::uint8_t z = 0;
    if (file.write(&z, 1) != 1) return DiskResult{DiskError::IoError};
    return DiskResult{DiskError::None};
}

static DiskResult write_blank_catalogue(fs::IFile& file, std::uint64_t sector0Offset,
                                        std::uint64_t sector1Offset,
                                        std::uint32_t sectorCount)
{
    // Write a minimal DFS 0.90 catalogue header (2 sectors).
    // Reference: https://beebwiki.mdfs.net/Acorn_DFS_disc_format
    //
    // Sector 0 bytes 0..7: title (first 8 chars), padded with NULs for DFS 0.90.
    // Sector 1 bytes 0..3: title (last 4 chars)
    // Sector 1 byte 4: cycle (BCD) - start at 0.
    // Sector 1 byte 5: file offset = 8 * file_count (0 for blank).
    // Sector 1 byte 6: bits 0..1 disc size high bits; bits 4..5 boot option (0).
    // Sector 1 byte 7: disc size low 8 bits.
    std::uint8_t sec0[256]{};
    std::uint8_t sec1[256]{};

    // Title "BLANK" (DFS permits up to 12 chars). We keep it short and NUL padded.
    std::memcpy(sec0 + 0, "BLANK", 5);

    // cycle=0, file_count=0 => file offset=0 already.
    // boot option=0 (none)
    const std::uint8_t disc_hi = static_cast<std::uint8_t>((sectorCount >> 8) & 0x03);
    sec1[6] = disc_hi; // other bits clear
    sec1[7] = static_cast<std::uint8_t>(sectorCount & 0xFF);

    if (!file.seek(sector0Offset) || file.write(sec0, sizeof(sec0)) != sizeof(sec0))
        return DiskResult{DiskError::IoError};
    if (!file.seek(sector1Offset) || file.write(sec1, sizeof(sec1)) != sizeof(sec1))
        return DiskResult{DiskError::IoError};
    return DiskResult{DiskError::None};
}

} // namespace fujinet::disk

