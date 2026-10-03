#pragma once

#include "fujinet/config/fuji_config.h"
#include "fujinet/core/core.h"
#include "fujinet/io/devices/network_device.h"
#include "fujinet/io/devices/network_protocol_registry.h"
#include "fujinet/core/wifi_service_init.h"

namespace fujinet::core {

void register_file_device(FujinetCore& core);
void register_host_service(FujinetCore& core);
void register_application_state_services(FujinetCore& core);

/// Register clock device without config persistence
void register_clock_device(FujinetCore& core);

/// Register clock device with config store for persistence
/// @param core The FujinetCore instance
/// @param configStore Non-owning pointer to config store for timezone persistence
void register_clock_device(FujinetCore& core, config::FujiConfigStore* configStore);

/// Register the NetworkDevice with the platform's protocol backends and the
/// `network:` settings from fujinet.yaml.
void register_network_device(FujinetCore& core, const config::NetworkConfig& config);
void register_network_device(FujinetCore& core,
                             io::ProtocolRegistry registry,
                             io::NetworkDeviceSettings settings);

/// fujinet.yaml `network:` values with 0 ("default") replaced by the platform's.
io::NetworkDeviceSettings network_device_settings(const config::NetworkConfig& config);
void register_disk_device(FujinetCore& core);
void register_modem_device(FujinetCore& core);

} // namespace fujinet::core
