#include "fujinet/platform/pico/stack_probe.h"

#include <cstdio>
#include <cstring>

using namespace fujinet::platform::pico;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main()
{
    const StackRegion r = core1_stack_region();
    const std::size_t size = static_cast<std::size_t>(r.top - r.bottom);
    std::memset(r.bottom, 0, size);
    CHECK(stack_min_free(r) == 0);

    paint_core1_stack();
    const std::size_t painted = stack_min_free(r);
    CHECK(painted > 0 && painted < size); // the top is left alone

    // A stack that grew down 100 bytes from the painted area's end has that much less headroom.
    std::memset(r.bottom + painted - 100, 0, 100);
    CHECK(stack_min_free(r) == painted - 100);

    // Writing the pattern value into used stack must not be mistaken for headroom beyond the first deviation.
    std::memset(r.bottom, 0, 8);
    CHECK(stack_min_free(r) == 0);
    return fails ? 1 : 0;
}
