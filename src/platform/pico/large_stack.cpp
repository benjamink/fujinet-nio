#include "fujinet/platform/large_stack.h"

namespace fujinet::platform {

// No spare stack memory to borrow on a 520 KB part: run on the caller's stack.
// Callers that need more than the core's stack (image decode) must be bounded
// by default_image_max_pixels() instead.
bool run_with_large_stack(std::size_t /*stackBytes*/, void (*fn)(void* ctx), void* ctx,
                          core::LargeStackReport* report)
{
    fn(ctx);
    if (report != nullptr) *report = core::LargeStackReport{};
    return true;
}

} // namespace fujinet::platform
