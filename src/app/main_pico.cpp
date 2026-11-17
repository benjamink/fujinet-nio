// FujiNet NIO on the Raspberry Pi Pico 2 W (RP2350 + CYW43439).
//
// A trimmed counterpart of main_posix.cpp: core, config, devices, Wi-Fi and the
// cooperative tick loop. What is stubbed is listed in pico2w/README.md.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>

#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "fujinet/platform/pico/memory_diag.h"
#include "fujinet/platform/pico/stack_probe.h"
#include "fujinet/platform/pico/tcp_diagnostics.h"
#include "fujinet/platform/pico/usb_service.h"
#include "fujinet/console/console_engine.h"
#include "fujinet/diag/diagnostic_provider.h"

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
    platform::pico::paint_core0_stack();
    if (!platform::pico::start_usb_worker()) {
        FN_LOGE(TAG, "USB worker startup failed; flash initialization prohibited");
        return 1;
    }
    if (cyw43_arch_init() != 0) {
        FN_LOGE(TAG, "CYW43 init failed");
        return 1;
    }
    FN_LOGI(TAG, "fujinet-nio starting (Pico 2 W)");
    FN_LOGI(TAG, "Version: %s", fujinet::version());

    core::FujinetCore core;
    const auto profile = build::current_build_profile();

    // LittleFS mounts the reserved final MiB; corrupt nonblank media stays untouched.
    if (auto flash = platform::pico::create_flash_filesystem()) {
        core.storageManager().registerFileSystem(std::move(flash));
    }
    // An SD card, if one is wired and present, becomes "sd0" and (as on ESP32) the default
    // persistent store for disk images and the app store; fujinet.yaml stays on flash.
    if (auto sd = platform::pico::create_sd_filesystem()) {
        core.storageManager().registerFileSystem(std::move(sd));
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

    // One CDC is exclusively owned by FujiBus; UART0 carries logs and console.
    auto channel = platform::create_channel_for_profile(profile, config, diskDev ? &diskDev->disk_service() : nullptr);
    if (channel) {
        core::setup_transports(core, *channel, profile, &config, nullptr);
    }

    diag::DiagnosticRegistry diagnostics;
    auto coreDiag=diag::create_core_diagnostic_provider(core);
    auto wifiContext=std::make_shared<diag::NetworkDiagWifiContext>();
    wifiContext->fuji=fujiConcrete; wifiContext->ensure_wifi=[&wifi] { return &wifi; };
    auto netDiag=diag::create_network_diagnostic_provider(core,wifiContext);
    auto diskDiag=diag::create_disk_diagnostic_provider(core);
    auto appDiag=diag::create_app_store_diagnostic_provider(core);
    diagnostics.add_provider(*coreDiag); diagnostics.add_provider(*netDiag);
    auto picoMem=platform::pico::create_memory_diagnostic_provider();
    diagnostics.add_provider(*diskDiag); diagnostics.add_provider(*appDiag); diagnostics.add_provider(*picoMem);
    auto consoleTransport=console::create_default_console_transport();
    console::ConsoleEngine console(diagnostics,*consoleTransport,core.storageManager());
    console.set_reboot_handler([] { watchdog_reboot(0,0,0); });
    console.set_format_handler([&core](std::string_view name) {
        if (name == "flash") return platform::pico::format_flash_filesystem();
        if (name == "sd0") {
            // The card's filesystem must not be open while it is recreated.
            core.storageManager().unregisterFileSystem("sd0");
            const bool ok = platform::pico::format_sd_filesystem();
            if (auto sd = platform::pico::create_sd_filesystem()) core.storageManager().registerFileSystem(std::move(sd));
            return ok;
        }
        return false;
    });
    // Debug aid: name the loop phase that stalls. FN_LOGW compiles away without FN_DEBUG.
    constexpr std::uint32_t kSlowPhaseUs = 20000;
    for (;;) {
        const std::uint32_t t0 = time_us_32();
        platform::pico::service_console();
        console.step(0);
        const std::uint32_t t1 = time_us_32();
        wifi.poll();
        const std::uint32_t t2 = time_us_32();
        core.tick();
        const std::uint32_t t3 = time_us_32();
        if (t1 - t0 > kSlowPhaseUs) FN_LOGW(TAG, "slow console phase: %lu us", static_cast<unsigned long>(t1 - t0));
        if (t2 - t1 > kSlowPhaseUs) FN_LOGW(TAG, "slow wifi.poll: %lu us", static_cast<unsigned long>(t2 - t1));
        if (t3 - t2 > kSlowPhaseUs) FN_LOGW(TAG, "slow core.tick: %lu us", static_cast<unsigned long>(t3 - t2));
        // Sparse stall/error evidence remains available without FN_DEBUG.
        // Its UART output is outside the CYW43/lwIP lock and normal traffic is silent.
        platform::pico::service_tcp_diagnostics();
        sleep_ms(1);
    }
}
