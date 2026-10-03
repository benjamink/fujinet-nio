#pragma once

#include <cstddef>
#include <string>

namespace fujinet::platform {

// Heap and stack headroom, for diagnostics (core.mem). Implemented in
// platform-specific .cpp files; POSIX has nothing useful to report.
struct MemoryStats {
    bool available{false};

    std::size_t internalFreeBytes{0};
    std::size_t internalMinFreeBytes{0};       // lowest since start-up
    std::size_t internalLargestBlockBytes{0};

    std::size_t psramFreeBytes{0};
    std::size_t psramMinFreeBytes{0};          // lowest since start-up
    std::size_t psramLargestBlockBytes{0};

    std::string taskName;                      // the task that asked
    std::size_t taskStackMinFreeBytes{0};      // its lowest free stack since it started
};

MemoryStats memory_stats();

} // namespace fujinet::platform
