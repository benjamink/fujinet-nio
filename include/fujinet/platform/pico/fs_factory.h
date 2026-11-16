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

std::unique_ptr<fujinet::fs::IFileSystem> create_tnfs_filesystem(bool useTcp = false);

std::unique_ptr<fujinet::fs::IFileSystem> create_http_filesystem();

} // namespace fujinet::platform::pico
