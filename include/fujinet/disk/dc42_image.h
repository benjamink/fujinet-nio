#pragma once

#include <memory>

#include "fujinet/disk/disk_image.h"

namespace fujinet::disk {

// Tags are ignored, and writes leave the header checksums stale.
std::unique_ptr<IDiskImage> make_dc42_disk_image();

struct Dc42Header {
    std::uint32_t dataBytes{0};
    std::uint32_t tagBytes{0};
};

// False unless the 84-byte header is DiskCopy 4.2 and its data fits in `fileBytes`.
bool parse_dc42_header(const std::uint8_t* header84, std::uint64_t fileBytes, Dc42Header& out) noexcept;

} // namespace fujinet::disk
