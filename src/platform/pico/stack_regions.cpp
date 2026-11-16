#include "fujinet/platform/pico/stack_probe.h"

// Linker-provided: core 0's stack is [__StackBottom, __StackTop), core 1's is
// [__StackOneBottom, __StackOneTop).
extern "C" char __StackBottom, __StackTop, __StackOneBottom, __StackOneTop;

namespace fujinet::platform::pico {

StackRegion core0_stack_region() { return {&__StackBottom, &__StackTop}; }
StackRegion core1_stack_region() { return {&__StackOneBottom, &__StackOneTop}; }

} // namespace fujinet::platform::pico
