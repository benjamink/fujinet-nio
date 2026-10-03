#include "fujinet/platform/image_translation.h"

namespace fujinet::platform {

std::uint32_t default_image_max_pixels()
{
    // stb_image holds the inflated data and the decoded image at once: peak
    // heap is about 4 bytes per source pixel for gray or palette PNGs, 6 for
    // RGB and 8 for RGBA. Measured on an ESP32-S3 N16R8 (net.translation.stats):
    // a 740x1215 gray PNG (0.9 Mpx) peaked at 3.7 MB with 8.2 MB free before.
    // 1024x1024 fits gray and RGB with room to spare; an RGBA image that large
    // fails its allocation and is reported as Unsupported.
    return 1024u * 1024u;
}

} // namespace fujinet::platform
