#include "fujinet/platform/image_translation.h"

namespace fujinet::platform {

// 520 KB of SRAM: bound decoding to what a small frame buffer allows.
std::uint32_t default_image_max_pixels()
{
    return 320u * 240u;
}

} // namespace fujinet::platform
