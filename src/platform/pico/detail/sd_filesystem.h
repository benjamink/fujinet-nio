#pragma once
#include <cstdint>
#include <memory>

#include "sd_card.h"
#include "fujinet/fs/filesystem.h"

namespace fujinet::platform::pico::detail {

// Seconds since the Unix epoch, or 0 if the clock is not set. Stamps new files.
using ClockFn = std::uint64_t (*)();

// FAT (FAT12/16/32, long names) on `card`, as filesystem "sd0". Mounts the card now; returns
// nullptr if it cannot (no card, unreadable, not FAT). The card and clock must outlive the result.
// One volume per process (FatFs is built with a single drive); core 0 only.
std::unique_ptr<fujinet::fs::IFileSystem> mount_sd_filesystem(SdCard& card, ClockFn clock);

// Create a fresh FAT filesystem on the whole card, destroying what is on it. Any filesystem
// from mount_sd_filesystem must be dropped first.
bool format_sd_card(SdCard& card);

} // namespace fujinet::platform::pico::detail
