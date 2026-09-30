#include "fujinet/disk/dc42_image.h"

namespace fujinet::disk {

namespace {

static constexpr std::uint64_t DC42_HEADER_BYTES = 0x54;
static constexpr std::uint16_t DC42_SECTOR_BYTES = 512;
static constexpr std::uint64_t DC42_DATA_CHECKSUM_OFFSET = 0x48;
static constexpr std::uint32_t DC42_TAG_BYTES_PER_SECTOR = 12;
static constexpr std::uint8_t DC42_MAX_DISK_FORMAT = 3; // 400K, 800K, 720K, 1440K

std::uint32_t be32(const std::uint8_t* p) noexcept
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

class Dc42DiskImage final : public IDiskImage {
public:
    ImageType type() const noexcept override { return ImageType::DiskCopy42; }
    DiskGeometry geometry() const noexcept override { return _geo; }
    bool read_only() const noexcept override { return _readOnly; }

    DiskResult mount(std::unique_ptr<fs::IFile> file, std::uint64_t sizeBytes, const MountOptions& opts) override
    {
        if (!file) return DiskResult{DiskError::OpenFailed};
        std::uint8_t header[DC42_HEADER_BYTES]{};
        Dc42Header h;
        if (!file->seek(0) || file->read(header, sizeof(header)) != sizeof(header) ||
            !parse_dc42_header(header, sizeBytes, h)) {
            return DiskResult{DiskError::BadImage};
        }
        _file = std::move(file);
        _readOnly = opts.readOnlyRequested;
        _geo = {};
        _geo.sectorSize = DC42_SECTOR_BYTES;
        _geo.sectorCount = h.dataBytes / DC42_SECTOR_BYTES;
        _stats = {};
        _checksumStale = false;
        return DiskResult{DiskError::None};
    }

    DiskResult unmount() override
    {
        _file.reset();
        _geo = {};
        _readOnly = true;
        _stats = {};
        _checksumStale = false;
        return DiskResult{DiskError::None};
    }

    DiskResult read_sector(std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes) override
    {
        const DiskResult r = check(lba, dst, dstBytes);
        if (!r.ok()) return r;
        ++_stats.readOps;
        ++_stats.seekOps;
        if (!_file->seek(offset_of(lba)) || _file->read(dst, DC42_SECTOR_BYTES) != DC42_SECTOR_BYTES) {
            return DiskResult{DiskError::IoError};
        }
        _stats.readBytes += DC42_SECTOR_BYTES;
        return DiskResult{DiskError::None, DC42_SECTOR_BYTES};
    }

    DiskResult write_sector(std::uint32_t lba, const std::uint8_t* src, std::size_t srcBytes) override
    {
        if (_file && _readOnly) return DiskResult{DiskError::ReadOnly};
        const DiskResult r = check(lba, src, srcBytes);
        if (!r.ok()) return r;
        ++_stats.writeOps;
        ++_stats.seekOps;
        if (!_file->seek(offset_of(lba)) || _file->write(src, DC42_SECTOR_BYTES) != DC42_SECTOR_BYTES) {
            return DiskResult{DiskError::IoError};
        }
        _stats.writeBytes += DC42_SECTOR_BYTES;
        _checksumStale = true;
        return DiskResult{DiskError::None, DC42_SECTOR_BYTES};
    }

    DiskResult flush() override
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        if (_checksumStale) {
            if (!rewrite_data_checksum()) return DiskResult{DiskError::IoError};
            _checksumStale = false;
        }
        return DiskResult{_file->flush() ? DiskError::None : DiskError::IoError};
    }

    DiskImageStats image_stats() const noexcept override { return _stats; }
    void reset_image_stats() noexcept override { _stats = {}; }

private:
    DiskResult check(std::uint32_t lba, const void* buf, std::size_t bytes) const
    {
        if (!_file) return DiskResult{DiskError::NotMounted};
        if (!buf || bytes < DC42_SECTOR_BYTES) return DiskResult{DiskError::InvalidRequest};
        if (lba >= _geo.sectorCount) return DiskResult{DiskError::OutOfRange};
        return DiskResult{DiskError::None};
    }

    // Tools such as DiskCopy check the data checksum, so keep it valid after
    // writes. Flush runs once per dirty session, so one pass over the data is
    // cheap; a sector-sized buffer keeps it small on constrained targets.
    bool rewrite_data_checksum()
    {
        std::uint8_t sector[DC42_SECTOR_BYTES];
        std::uint32_t sum = 0;
        for (std::uint32_t lba = 0; lba < _geo.sectorCount; ++lba) {
            if (!_file->seek(offset_of(lba)) || _file->read(sector, sizeof(sector)) != sizeof(sector)) {
                return false;
            }
            sum = dc42_checksum_update(sum, sector, sizeof(sector));
        }
        const std::uint8_t be[4] = {
            std::uint8_t(sum >> 24), std::uint8_t(sum >> 16), std::uint8_t(sum >> 8), std::uint8_t(sum)};
        return _file->seek(DC42_DATA_CHECKSUM_OFFSET) && _file->write(be, sizeof(be)) == sizeof(be);
    }

    static std::uint64_t offset_of(std::uint32_t lba) noexcept
    {
        return DC42_HEADER_BYTES + std::uint64_t(lba) * DC42_SECTOR_BYTES;
    }

    std::unique_ptr<fs::IFile> _file;
    DiskGeometry _geo{};
    bool _readOnly{true};
    DiskImageStats _stats{};
    bool _checksumStale{false};
};

} // namespace

bool parse_dc42_header(const std::uint8_t* h, std::uint64_t fileBytes, Dc42Header& out) noexcept
{
    // Pascal name (64 bytes), data size, tag size, checksums, disk format,
    // format byte, magic 0x0100. This probe runs before the FAT and SSD ones,
    // and a wrong claim shifts every sector by the header, so check every
    // field with a fixed meaning.
    if (fileBytes < DC42_HEADER_BYTES || h[0] > 63) return false;
    if (h[0x52] != 0x01 || h[0x53] != 0x00) return false;
    if (h[0x50] > DC42_MAX_DISK_FORMAT) return false;
    const std::uint32_t data = be32(h + 0x40);
    const std::uint32_t tags = be32(h + 0x44);
    if (data == 0 || data % DC42_SECTOR_BYTES != 0) return false;
    if (tags != 0 && tags != (data / DC42_SECTOR_BYTES) * DC42_TAG_BYTES_PER_SECTOR) return false;
    if (DC42_HEADER_BYTES + std::uint64_t(data) + tags > fileBytes) return false;
    out.dataBytes = data;
    out.tagBytes = tags;
    return true;
}

std::uint32_t dc42_checksum_update(std::uint32_t sum, const std::uint8_t* bytes, std::size_t len) noexcept
{
    for (std::size_t i = 0; i + 1 < len; i += 2) {
        sum += (std::uint32_t(bytes[i]) << 8) | bytes[i + 1];
        sum = (sum >> 1) | (sum << 31);
    }
    return sum;
}

std::unique_ptr<IDiskImage> make_dc42_disk_image()
{
    return std::make_unique<Dc42DiskImage>();
}

} // namespace fujinet::disk
