// Host stand-in for src/platform/pico/stack_regions.cpp: the stacks are plain buffers.
#include "fujinet/platform/pico/stack_probe.h"

namespace fujinet::platform::pico {
namespace {
alignas(8) char g_stack0[4096];
alignas(8) char g_stack1[1024];
} // namespace

StackRegion core0_stack_region() { return {g_stack0, g_stack0 + sizeof g_stack0}; }
StackRegion core1_stack_region() { return {g_stack1, g_stack1 + sizeof g_stack1}; }
} // namespace fujinet::platform::pico
