#pragma once

#include <memory>

#include "fujinet/fs/filesystem.h"

namespace fujinet::platform::pico {

// Flash-backed filesystem. Not implemented yet: returns nullptr (README, "Filesystem").
std::unique_ptr<fujinet::fs::IFileSystem> create_flash_filesystem();

std::unique_ptr<fujinet::fs::IFileSystem> create_tnfs_filesystem(bool useTcp = false);

std::unique_ptr<fujinet::fs::IFileSystem> create_http_filesystem();

} // namespace fujinet::platform::pico
