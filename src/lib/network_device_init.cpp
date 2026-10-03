#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/core/device_init.h"
#include "fujinet/io/devices/network_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/core/logging.h"
#include "fujinet/platform/image_translation.h"
#include "fujinet/platform/network_registry.h"

namespace fujinet::core {

using fujinet::io::DeviceID;
using fujinet::io::NetworkDevice;
using fujinet::io::ProtocolRegistry;
using fujinet::io::protocol::WireDeviceId;
using fujinet::io::protocol::to_device_id;

static constexpr const char* TAG = "core";

io::NetworkDeviceSettings network_device_settings(const config::ContentTranslationConfig& config)
{
    io::NetworkDeviceSettings settings;
    settings.imageMaxPixels = config.image.maxPixels != 0
        ? config.image.maxPixels
        : fujinet::platform::default_image_max_pixels();
    return settings;
}

void register_network_device(FujinetCore& core, ProtocolRegistry registry, io::NetworkDeviceSettings settings)
{
    // IMPORTANT: only register ONE network device for now.
    // We can later work out how/if we need to scale out without allocating/registering
    // multiple device instances up front.
    auto dev = std::make_unique<NetworkDevice>(std::move(registry), settings);
    DeviceID id = to_device_id(WireDeviceId::NetworkService); // 0xFD

    bool ok = core.deviceManager().registerDevice(id, std::move(dev));
    if (!ok) {
        FN_LOGE(TAG, "Failed to register NetworkDevice on DeviceID %u", static_cast<unsigned>(id));
    } else {
        FN_ELOG("Registered NetworkDevice on DeviceID %u (image_max_pixels=%u)",
                static_cast<unsigned>(id),
                static_cast<unsigned>(settings.imageMaxPixels));
    }
}

void register_network_device(FujinetCore& core, const config::ContentTranslationConfig& config)
{
    auto reg = fujinet::platform::make_default_network_registry();
    register_network_device(core, std::move(reg), network_device_settings(config));
}

} // namespace fujinet::core
