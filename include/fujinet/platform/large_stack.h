#pragma once

#include <cstddef>

namespace fujinet::platform {

// core::LargeStackRunner for this platform: runs fn(ctx) on a temporary
// thread or task whose stack is stackBytes, and waits for it. The stack only
// exists for the call. Implemented in platform-specific .cpp files
// (POSIX: a pthread; ESP32: a FreeRTOS task with its stack in PSRAM).
bool run_with_large_stack(std::size_t stackBytes, void (*fn)(void* ctx), void* ctx);

} // namespace fujinet::platform
