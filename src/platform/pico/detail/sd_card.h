#pragma once
// An SD/SDHC/SDXC card driven in SPI mode (SD Physical Layer spec, "SPI Mode"). Hardware access
// is behind SpiBus, so the protocol is tested natively against a simulated card.
#include <cstddef>
#include <cstdint>

namespace fujinet::platform::pico::detail {

class SpiBus {
public:
    virtual ~SpiBus() = default;
    virtual void set_clock_hz(std::uint32_t hz) = 0;
    virtual void select(bool selected) = 0;               // chip select (active low on the wire)
    virtual std::uint8_t transfer(std::uint8_t out) = 0;  // one byte, full duplex
    virtual void read(std::uint8_t* dst, std::size_t n) = 0;        // clocks out 0xFF
    virtual void write(const std::uint8_t* src, std::size_t n) = 0; // discards the input
    virtual std::uint32_t now_ms() = 0;
    virtual void delay_ms(std::uint32_t ms) = 0;
};

constexpr std::uint32_t kSdSectorSize = 512;

class SdCard {
public:
    // init_hz: clock during initialisation (the spec requires 100-400 kHz); run_hz afterwards.
    SdCard(SpiBus& bus, std::uint32_t run_hz, std::uint32_t init_hz = 400000)
        : bus_(bus), run_hz_(run_hz), init_hz_(init_hz) {}

    // Bring the card from power-up to transfer state. Safe to call again after an error.
    bool init();
    bool ready() const { return ready_; }
    std::uint32_t sector_count() const { return sectors_; }
    std::uint32_t now_ms() { return bus_.now_ms(); }
    bool high_capacity() const { return block_addressing_; }

    // Return false on any error; the card is then marked not ready (call init() to recover).
    bool read(std::uint32_t lba, std::uint8_t* dst, std::uint32_t count);
    bool write(std::uint32_t lba, const std::uint8_t* src, std::uint32_t count);
    // Wait until the card has finished any programming it is doing.
    bool sync();

private:
    SpiBus& bus_;
    std::uint32_t run_hz_, init_hz_;
    bool ready_ = false, block_addressing_ = false;
    std::uint32_t sectors_ = 0;

    bool wait_not_busy(std::uint32_t timeout_ms);
    std::uint8_t command(std::uint8_t index, std::uint32_t arg);  // returns R1, 0xFF on timeout
    std::uint8_t app_command(std::uint8_t index, std::uint32_t arg);
    bool read_data(std::uint8_t* dst, std::size_t len, std::uint32_t timeout_ms);
    bool write_block(const std::uint8_t* src, std::uint8_t token);
    bool read_csd();
    bool fail();  // deselect, mark not ready
    void release();
    std::uint32_t address(std::uint32_t lba) const { return block_addressing_ ? lba : lba * kSdSectorSize; }
};

} // namespace fujinet::platform::pico::detail
