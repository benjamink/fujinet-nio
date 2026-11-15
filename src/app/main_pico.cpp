// FujiNet NIO on the Raspberry Pi Pico 2 W (RP2350 + CYW43439).
//
// A trimmed counterpart of main_posix.cpp: core, config, devices, Wi-Fi and the
// cooperative tick loop. What is stubbed is listed in pico2w/README.md.
#include <cstdio>
#include <memory>

#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "fujinet/build/profile.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/core/device_init.h"
#include "fujinet/core/logging.h"
#include "fujinet/core/version.h"
#include "fujinet/fs/boot_mount.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/hotsync/hotsync_init.h"
#include "fujinet/io/devices/disk_device.h"
#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/platform/channel_factory.h"
#include "fujinet/platform/fuji_device_factory.h"
#include "fujinet/platform/pico/fs_factory.h"
#include "fujinet/platform/pico/wifi_link.h"

using namespace fujinet;
using namespace fujinet::io::protocol;

static constexpr const char* TAG = "nio";

int main()
{
    stdio_init_all();
    if (cyw43_arch_init() != 0) {
        FN_LOGE(TAG, "CYW43 init failed");
        return 1;
    }
    FN_LOGI(TAG, "fujinet-nio starting (Pico 2 W)");
    FN_LOGI(TAG, "Version: %s", fujinet::version());

    core::FujinetCore core;
    const auto profile = build::current_build_profile();

    // Filesystems. "flash" is not implemented yet and yields nullptr.
    if (auto flash = platform::pico::create_flash_filesystem()) {
        core.storageManager().registerFileSystem(std::move(flash));
    }
    core.storageManager().registerFileSystem(platform::pico::create_tnfs_filesystem());
    core.storageManager().registerFileSystem(platform::pico::create_http_filesystem());

    platform::FujiDeviceHooks hooks{.onReset = [] { watchdog_reboot(0, 0, 0); }};
    auto fuji = platform::create_fuji_device(core, profile, hooks);
    auto* fujiConcrete = dynamic_cast<io::FujiDevice*>(fuji.get());
    if (!fujiConcrete || !core.deviceManager().registerDevice(to_device_id(WireDeviceId::FujiNet), std::move(fuji))) {
        FN_LOGE(TAG, "Could not register the FujiDevice");
        return 1;
    }
    fujiConcrete->start();
    const auto& config = fujiConcrete->config();

    platform::pico::PicoWifiLink wifi;
    if (config.wifi.enabled && !config.wifi.ssid.empty()) {
        wifi.set_bssid(config.wifi.bssid);
        wifi.connect(config.wifi.ssid, config.wifi.passphrase);
    }

    core::register_host_service(core);
    core::register_file_device(core);
    core::register_application_state_services(core);
    core::register_clock_device(core, fujiConcrete->config_store());
    core::register_disk_device(core);
    core::register_wifi_service(core, fujiConcrete->config_mut(), fujiConcrete->config_store(),
                                [&wifi]() { return &wifi; });
    core::register_network_device(core, config.translation);
    core::register_modem_device(core);

    auto* diskDev = dynamic_cast<io::DiskDevice*>(
        core.deviceManager().getDevice(to_device_id(WireDeviceId::DiskService)));
    if (diskDev) {
        diskDev->restore_runtime_mounts();
        apply_boot_mount(diskDev->disk_service(), core.storageManager(), config.boot, 0);
    }

    // The transport: no channel exists for this profile yet, so the core runs
    // without one (README, "Channel"). Everything above is what gets measured.
    auto channel = platform::create_channel_for_profile(profile, config, diskDev ? &diskDev->disk_service() : nullptr);
    if (channel) {
        core::setup_transports(core, *channel, profile, &config, nullptr);
    }

    for (;;) {
        wifi.poll();
        core.tick();
        sleep_ms(1);
    }
}
