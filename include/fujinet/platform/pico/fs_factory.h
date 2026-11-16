#pragma once

#include <memory>

#include "fujinet/fs/filesystem.h"

namespace fujinet::platform::pico {

// LittleFS on the reserved final MiB of flash. Returns nullptr if it cannot be
// mounted; blank storage is formatted, nonblank storage is left untouched.
std::unique_ptr<fujinet::fs::IFileSystem> create_flash_filesystem();

// Recreate that LittleFS empty. Used by the console `format flash confirm` command,
// the deliberate recovery for storage create_flash_filesystem refused to touch.
bool format_flash_filesystem();

// FAT filesystem "sd0" on an SD card wired to the SPI pins in pico2w/CMakeLists.txt. Returns
// nullptr when no usable card is present at the time of the call (a card inserted later needs a
// restart); a card that was mounted and then glitches is re-initialised on the next access.
std::unique_ptr<fujinet::fs::IFileSystem> create_sd_filesystem();

// Create an empty FAT filesystem on the whole card, destroying its contents. The console
// `format sd0 confirm` command. Drop the filesystem from the storage manager first.
bool format_sd_filesystem();

std::unique_ptr<fujinet::fs::IFileSystem> create_tnfs_filesystem(bool useTcp = false);

std::unique_ptr<fujinet::fs::IFileSystem> create_http_filesystem();

} // namespace fujinet::platform::pico
