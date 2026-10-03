#pragma once

#include "fujinet/io/devices/network_protocol_registry.h"

#include <cstdint>

namespace fujinet::platform {

// Build a default URL-scheme -> protocol backend registry for the current platform.
// Implemented in platform-specific .cpp files (POSIX / ESP32).
io::ProtocolRegistry make_default_network_registry();

// Largest source image (width*height) the Image translator decodes when
// fujinet.yaml leaves network.image_max_pixels at 0. Sized to the platform's
// memory; implemented next to make_default_network_registry().
std::uint32_t default_image_max_pixels();

} // namespace fujinet::platform
