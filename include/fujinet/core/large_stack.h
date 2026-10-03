#pragma once

#include <cstddef>

namespace fujinet::core {

// Runs fn(ctx) to completion on a stack of at least stackBytes, then returns
// true. Returns false, without calling fn, when no such stack can be had.
//
// For occasional work that needs far more stack than the task calling it
// (image decoding), so that task's own stack stays small. The platform
// supplies it: platform::run_with_large_stack().
using LargeStackRunner = bool (*)(std::size_t stackBytes, void (*fn)(void* ctx), void* ctx);

} // namespace fujinet::core
