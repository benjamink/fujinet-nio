#include "fujinet/platform/pico/stack_probe.h"

#include <cstring>

namespace fujinet::platform::pico {
namespace {

constexpr unsigned char kPaint = 0xA5;
constexpr std::size_t kKeepAboveStack = 256; // do not paint over frames already in use

} // namespace

void paint_core0_stack()
{
    const StackRegion region = core0_stack_region();
    char here;
    char* limit = &here - kKeepAboveStack;
    if (limit > region.bottom) std::memset(region.bottom, kPaint, static_cast<std::size_t>(limit - region.bottom));
}

void paint_core1_stack()
{
    const StackRegion region = core1_stack_region();
    const std::size_t size = static_cast<std::size_t>(region.top - region.bottom);
    if (size > kKeepAboveStack) std::memset(region.bottom, kPaint, size - kKeepAboveStack);
}

std::size_t stack_min_free(const StackRegion& region)
{
    const auto* p = reinterpret_cast<const unsigned char*>(region.bottom);
    const auto* end = reinterpret_cast<const unsigned char*>(region.top);
    while (p < end && *p == kPaint) ++p;
    return static_cast<std::size_t>(p - reinterpret_cast<const unsigned char*>(region.bottom));
}

} // namespace fujinet::platform::pico
