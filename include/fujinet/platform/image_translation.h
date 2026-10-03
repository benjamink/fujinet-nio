#pragma once

#include <cstdint>

namespace fujinet::platform {

// Largest source image (width*height) the Image translator decodes when
// fujinet.yaml leaves translation.image.max_pixels at 0. Sized to the
// platform's memory. Implemented in platform-specific .cpp files (POSIX / ESP32).
std::uint32_t default_image_max_pixels();

} // namespace fujinet::platform
