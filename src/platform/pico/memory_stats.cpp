#include "fujinet/platform/memory_stats.h"

#include <malloc.h>

namespace fujinet::platform {

namespace {
extern "C" char __StackLimit, __bss_end__;
}

// newlib heap: total arena less what mallinfo reports in use, up to the stack limit.
MemoryStats memory_stats()
{
    MemoryStats s{};
    const struct mallinfo mi = mallinfo();
    const std::size_t arena_total = static_cast<std::size_t>(&__StackLimit - &__bss_end__);
    s.available = true;
    s.internalFreeBytes = arena_total > static_cast<std::size_t>(mi.uordblks)
                              ? arena_total - static_cast<std::size_t>(mi.uordblks) : 0;
    s.internalMinFreeBytes = 0; // not tracked yet
    s.internalLargestBlockBytes = static_cast<std::size_t>(mi.fordblks);
    s.taskName = "main";
    return s;
}

} // namespace fujinet::platform
