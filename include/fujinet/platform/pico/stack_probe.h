#pragma once
// Stack high-water measurement. A stack is filled with a known pattern before it is used; the
// bytes at its low end that still hold the pattern are the headroom the stack never reached.
#include <cstddef>

namespace fujinet::platform::pico {

// A stack grows down from `top` towards `bottom`.
struct StackRegion {
    char* bottom;
    char* top;
};

// Where the two cores' stacks are. Firmware: from the linker script (stack_regions.cpp).
StackRegion core0_stack_region();
StackRegion core1_stack_region();

// Fill core 0's unused stack with the pattern. Call once, early in main: the frames already in use
// (and a margin above them) are left alone.
void paint_core0_stack();
// Fill core 1's stack. Call before launching core 1, so it is entirely unused; the top is left alone
// because launching core 1 writes its first frame there.
void paint_core1_stack();

// Bytes of the region's low end still holding the pattern: how much headroom the stack has had.
std::size_t stack_min_free(const StackRegion& region);

} // namespace fujinet::platform::pico
