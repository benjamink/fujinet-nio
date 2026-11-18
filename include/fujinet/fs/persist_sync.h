#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "fujinet/fs/filesystem.h"

namespace fujinet::fs {

// One-way copy of the persistent data between two local filesystems, so a card added after
// the device has been in use can be seeded from flash ("restore"), or the card can be backed
// up to flash. It is always explicit: nothing calls it on its own, and the direction is the
// caller's choice. It never merges and never deletes: a file that exists only in the
// destination is left alone.
//
// What is copied (the data an application or the user would not want to set up again):
//   /FujiNet/...      appkeys and the app store
//   /fujinet.yaml     the device configuration, including wifi credentials
// Disk images and everything else are not touched.
enum class SyncAction {
    Copy,       // the destination has no such file
    Overwrite,  // the destination differs and overwrite was requested
    Identical,  // same size and bytes; nothing to do
    Conflict,   // the destination differs and overwrite was not requested; left as it is
    Failed,     // could not be read or written
};

struct PersistSyncOptions {
    bool apply{false};      // false: only report what would happen (dry run)
    bool overwrite{false};  // replace a destination file that differs from the source
};

struct PersistSyncSummary {
    // False when the sync could not run (see error) or any file failed.
    bool ok{false};
    // Why it could not run; empty otherwise.
    std::string error;
    std::size_t copied{0};       // Copy: new files (would be, in a dry run)
    std::size_t overwritten{0};  // Overwrite
    std::size_t identical{0};
    std::size_t conflicts{0};
    std::size_t failed{0};
};

// Called once per file with its path, so a caller can show progress or a dry-run listing.
using SyncObserver = std::function<void(const std::string& path, SyncAction action)>;

// Copies the persistent data from `from` to `to`. Both must be local flash or SD filesystems,
// distinct from each other, and available(). A file is written under a temporary name and
// renamed into place, so a failure part-way leaves the old destination file in one piece.
PersistSyncSummary sync_persistent(IFileSystem& from,
                                   IFileSystem& to,
                                   const PersistSyncOptions& options,
                                   const SyncObserver& observe = {});

} // namespace fujinet::fs
