#include "fujinet/platform/image_translation.h"

namespace fujinet::platform {

std::uint32_t default_image_max_pixels()
{
    // Memory is plentiful; this bounds the work per image, not the RAM.
    return 4096u * 4096u;
}

} // namespace fujinet::platform
