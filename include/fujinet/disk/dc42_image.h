#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "fujinet/disk/disk_image.h"

namespace fujinet::disk {

// Tags are ignored. Flushing after writes recomputes the header's data
// checksum; the tag checksum stays valid because tags are never written.
std::unique_ptr<IDiskImage> make_dc42_disk_image();

struct Dc42Header {
    std::uint32_t dataBytes{0};
    std::uint32_t tagBytes{0};
};

// False unless the 84-byte header is DiskCopy 4.2 and its data fits in `fileBytes`.
bool parse_dc42_header(const std::uint8_t* header84, std::uint64_t fileBytes, Dc42Header& out) noexcept;

// Folds `bytes` (even length) into a DiskCopy 4.2 checksum: each big-endian
// 16-bit word is added, then the sum is rotated right by one bit. Start at 0.
std::uint32_t dc42_checksum_update(std::uint32_t sum, const std::uint8_t* bytes, std::size_t len) noexcept;

} // namespace fujinet::disk
