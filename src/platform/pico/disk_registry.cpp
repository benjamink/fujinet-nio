#include "fujinet/platform/disk_registry.h"

namespace fujinet::platform {

disk::ImageRegistry make_default_disk_image_registry()
{
    return disk::make_default_image_registry();
}

} // namespace fujinet::platform
