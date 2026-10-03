#include "fujinet/platform/image_translation.h"

namespace fujinet::platform {

std::uint32_t default_image_max_pixels()
{
    // stb_image's PNG path holds the compressed IDAT copy, the inflate buffer
    // and the decoded RGB at once, roughly 3-4 bytes per pixel at peak, on top
    // of the cached response body. 700x700 (0.49 Mpx) keeps that near 2 MB of
    // PSRAM. See docs/network_device_protocol.md for the measured time.
    return 700u * 700u;
}

} // namespace fujinet::platform
