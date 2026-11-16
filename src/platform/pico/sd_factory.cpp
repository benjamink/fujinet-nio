#include "detail/sd_filesystem.h"
#include "fujinet/core/logging.h"
#include "fujinet/platform/pico/fs_factory.h"
#include "fujinet/platform/time.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"

// Wiring and speed come from pico2w/CMakeLists.txt (NIO_PICO_SD_*).
#ifndef NIO_PICO_SD_SPI
#define NIO_PICO_SD_SPI 0
#define NIO_PICO_SD_MISO 16
#define NIO_PICO_SD_CS 17
#define NIO_PICO_SD_SCK 18
#define NIO_PICO_SD_MOSI 19
#define NIO_PICO_SD_BAUD 10000000
#endif

namespace fujinet::platform::pico {
namespace {

constexpr const char* TAG = "sd";

class PicoSpiBus final : public detail::SpiBus {
    spi_inst_t* spi_ = NIO_PICO_SD_SPI == 0 ? spi0 : spi1;
public:
    PicoSpiBus() {
        spi_init(spi_, 400000);
        spi_set_format(spi_, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        gpio_set_function(NIO_PICO_SD_SCK, GPIO_FUNC_SPI);
        gpio_set_function(NIO_PICO_SD_MOSI, GPIO_FUNC_SPI);
        gpio_set_function(NIO_PICO_SD_MISO, GPIO_FUNC_SPI);
        gpio_pull_up(NIO_PICO_SD_MISO); // an absent card then reads as 0xFF, not noise
        gpio_init(NIO_PICO_SD_CS);
        gpio_set_dir(NIO_PICO_SD_CS, GPIO_OUT);
        gpio_put(NIO_PICO_SD_CS, 1);
    }
    void set_clock_hz(std::uint32_t hz) override { spi_set_baudrate(spi_, hz); }
    void select(bool on) override { gpio_put(NIO_PICO_SD_CS, on ? 0 : 1); }
    std::uint8_t transfer(std::uint8_t out) override {
        std::uint8_t in = 0xFF;
        spi_write_read_blocking(spi_, &out, &in, 1);
        return in;
    }
    void read(std::uint8_t* dst, std::size_t n) override { spi_read_blocking(spi_, 0xFF, dst, n); }
    void write(const std::uint8_t* src, std::size_t n) override { spi_write_blocking(spi_, src, n); }
    std::uint32_t now_ms() override { return to_ms_since_boot(get_absolute_time()); }
    void delay_ms(std::uint32_t ms) override { sleep_ms(ms); }
};

detail::SdCard& card() {
    static PicoSpiBus bus;
    static detail::SdCard sd(bus, NIO_PICO_SD_BAUD);
    return sd;
}

} // namespace

std::unique_ptr<fujinet::fs::IFileSystem> create_sd_filesystem()
{
    auto fs = detail::mount_sd_filesystem(card(), &fujinet::platform::unix_time_seconds);
    if (fs) {
        FN_LOGI(TAG, "SD card mounted: %lu sectors, %s", static_cast<unsigned long>(card().sector_count()),
                card().high_capacity() ? "SDHC/SDXC" : "SDSC");
    } else {
        FN_LOGW(TAG, "no usable SD card (wired to SPI%d: MISO GP%d CS GP%d SCK GP%d MOSI GP%d)", NIO_PICO_SD_SPI,
                NIO_PICO_SD_MISO, NIO_PICO_SD_CS, NIO_PICO_SD_SCK, NIO_PICO_SD_MOSI);
    }
    return fs;
}

bool format_sd_filesystem() { return detail::format_sd_card(card()); }

} // namespace fujinet::platform::pico
