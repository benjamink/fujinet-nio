#include "fujinet/platform/memory_stats.h"

namespace fujinet::platform {

// The host's allocator and an 8 MB main stack have nothing worth reporting.
MemoryStats memory_stats()
{
    return MemoryStats{};
}

} // namespace fujinet::platform
