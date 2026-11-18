#include "fujinet/platform/esp32/fs_init.h"
#include "fujinet/core/logging.h"
#include "fujinet/platform/esp32/pinmap.h"

extern "C" {
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_vfs_fat.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_timer.h"
}

namespace fujinet::platform::esp32 {

static constexpr const char* TAG = "platform";

// Keep a static card handle so we can unmount if needed later
static sdmmc_card_t* s_sdcard = nullptr;
// True once init_sdcard_spi() found SD pins; a board without them never retries a mount.
static bool s_sd_configured = false;

bool init_littlefs()
{
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/fujifs";           // mount point
    conf.partition_label = "storage";     // we'll define this partition
    conf.format_if_mount_failed = true;   // format on first boot if missing
    conf.read_only = false;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            FN_LOGE(TAG, "Failed to mount or format LittleFS");
        } else if (err == ESP_ERR_NOT_FOUND) {
            FN_LOGE(TAG, "LittleFS partition label '%s' not found", conf.partition_label);
        } else {
            FN_LOGE(TAG, "esp_vfs_littlefs_register failed: 0x%x", static_cast<unsigned>(err));
        }
        return false;
    }

    size_t total = 0, used = 0;
    err = esp_littlefs_info(conf.partition_label, &total, &used);
    if (err != ESP_OK) {
        FN_LOGW(TAG, "Failed to get LittleFS info (err=0x%x)", static_cast<unsigned>(err));
    } else {
        FN_ELOG("LittleFS mounted at %s, total=%u, used=%u",
                 conf.base_path,
                 static_cast<unsigned>(total),
                 static_cast<unsigned>(used));
    }

    return true;
}

namespace {

constexpr const char* kSdMountPoint = "/sdcard";
// A pulled card is retried this often, so an empty slot does not cost a failed mount per query.
constexpr int64_t kSdRetryUs = 1000 * 1000;

bool s_sd_bus_ready = false;
int64_t s_sd_last_attempt_us = 0;

// SPI bus + FAT mount of whatever card is in the slot. The bus is set up once; the mount is
// repeatable, which is what lets a card put back after removal come back.
esp_err_t mount_sdcard()
{
    using fujinet::platform::esp32::pinmap;
    const auto& pins = pinmap().sd;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;  // keep consistent with the bus init
    host.max_freq_khz = pins.max_freq_khz;

    if (!s_sd_bus_ready) {
        spi_bus_config_t bus_cfg = {};
        bus_cfg.mosi_io_num = pins.mosi;
        bus_cfg.miso_io_num = pins.miso;
        bus_cfg.sclk_io_num = pins.sck;
        bus_cfg.quadwp_io_num = -1;
        bus_cfg.quadhd_io_num = -1;
        bus_cfg.max_transfer_sz = 4000;

        esp_err_t err = spi_bus_initialize(static_cast<spi_host_device_t>(host.slot),
                                           &bus_cfg, SDSPI_DEFAULT_DMA);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            FN_LOGE(TAG, "spi_bus_initialize failed: 0x%x", static_cast<unsigned>(err));
            return err;
        }
        s_sd_bus_ready = true;
    }

    // Only CS and the host id are per-card.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = static_cast<gpio_num_t>(pins.cs);
    slot_config.host_id = static_cast<spi_host_device_t>(host.slot);

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed   = false,
        .max_files                = 32,
        .allocation_unit_size     = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat              = false,
    };

    return esp_vfs_fat_sdspi_mount(kSdMountPoint, &host, &slot_config, &mount_config, &s_sdcard);
}

} // namespace

bool sdcard_present() noexcept
{
    if (s_sdcard) {
        // sdmmc_get_status() sends SEND_STATUS (CMD13) and fails when the card does not answer.
        if (sdmmc_get_status(s_sdcard) == ESP_OK) {
            return true;
        }
        // Gone: release the mount so a card put back can be mounted afresh.
        esp_vfs_fat_sdcard_unmount(kSdMountPoint, s_sdcard);
        s_sdcard = nullptr;
        s_sd_last_attempt_us = esp_timer_get_time();
        return false;
    }

    // Never configured (no SD pins on this board): nothing to retry.
    if (!s_sd_configured) {
        return false;
    }

    const int64_t now = esp_timer_get_time();
    if (now - s_sd_last_attempt_us < kSdRetryUs) {
        return false;
    }
    s_sd_last_attempt_us = now;
    return mount_sdcard() == ESP_OK;
}

bool sdcard_mounted() noexcept
{
    return s_sdcard != nullptr;
}

bool init_sdcard_spi()
{
    using fujinet::platform::esp32::pinmap;

    const auto& pins = pinmap().sd;

    // USB-only boards have no SD wiring. Do not pass disabled GPIOs to SDSPI.
    if (pins.mosi < 0 || pins.miso < 0 || pins.sck < 0 || pins.cs < 0) {
        FN_LOGI(TAG, "SD card disabled: SPI pin mapping is incomplete");
        return false;
    }
    s_sd_configured = true;

    FN_ELOG(
        "Initializing SD card over SPI: MOSI=%d MISO=%d SCK=%d CS=%d",
        pins.mosi, pins.miso, pins.sck, pins.cs);

    const esp_err_t err = mount_sdcard();
    s_sd_last_attempt_us = esp_timer_get_time();
    if (err != ESP_OK) {
        FN_LOGW(TAG, "Failed to mount SD card at %s (err=0x%x)",
                kSdMountPoint, static_cast<unsigned>(err));
        FN_ELOG("SD card mount failed at %s: %s (0x%x)",
                kSdMountPoint, esp_err_to_name(err), static_cast<unsigned>(err));
        return false;
    }

    // Optional but handy while debugging:
    sdmmc_card_print_info(stdout, s_sdcard);
    FN_ELOG("SD card mounted at %s", kSdMountPoint);
    return true;
}

} // namespace fujinet::platform::esp32
