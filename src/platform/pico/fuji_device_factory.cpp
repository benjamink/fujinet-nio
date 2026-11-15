#include "fujinet/platform/fuji_device_factory.h"

#include "fujinet/core/core.h"
#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/platform/fuji_config_store_factory.h"

namespace fujinet::platform {

std::unique_ptr<io::VirtualDevice>
create_fuji_device(core::FujinetCore& core, const build::BuildProfile& /*profile*/, const FujiDeviceHooks& hooks)
{
    auto reset = hooks.onReset ? hooks.onReset : [] {};
    return std::make_unique<io::FujiDevice>(std::move(reset), create_fuji_config_store(core.storageManager()));
}

} // namespace fujinet::platform
