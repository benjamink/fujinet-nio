#pragma once

#include <memory>
#include <string>
#include "fujinet/fs/filesystem.h"

namespace fujinet::fs {

// Optional platform hook, called after each successful read or write.
using FileActivityHook = void (*)() noexcept;

// Optional platform hook behind IFileSystem::available(): true while the medium under the
// filesystem answers (for an SD card, that it is still in its slot). Without one the
// filesystem is always available.
using AvailabilityProbe = bool (*)() noexcept;

// Generic stdio-backed filesystem: works on POSIX and ESP-IDF VFS.
std::unique_ptr<IFileSystem>
create_stdio_filesystem(const std::string& rootDir,
                        const std::string& name,
                        FileSystemKind kind,
                        FileActivityHook onActivity = nullptr,
                        AvailabilityProbe isAvailable = nullptr);

} // namespace fujinet::fs
