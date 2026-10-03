#pragma once

#include <cstddef>

namespace fujinet::core {

// What a LargeStackRunner measured while fn ran. Fields the platform cannot
// measure stay 0.
struct LargeStackReport {
    std::size_t stackUsedBytes{0};          // deepest the temporary stack went
    std::size_t heapFreeBeforeBytes{0};     // free heap when fn started
    std::size_t heapLargestBlockBytes{0};   // largest free heap block when fn started
    std::size_t heapLowestFreeBytes{0};     // lowest free heap while fn ran
};

// Runs fn(ctx) to completion on a stack of at least stackBytes, then returns
// true. Returns false, without calling fn, when no such stack can be had.
// report, when not null, gets what was measured.
//
// For occasional work that needs far more stack than the task calling it
// (image decoding), so that task's own stack stays small. The platform
// supplies it: platform::run_with_large_stack().
using LargeStackRunner = bool (*)(std::size_t stackBytes,
                                  void (*fn)(void* ctx),
                                  void* ctx,
                                  LargeStackReport* report);

} // namespace fujinet::core
