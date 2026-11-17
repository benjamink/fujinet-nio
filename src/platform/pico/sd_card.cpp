#include "detail/sd_card.h"

namespace fujinet::platform::pico::detail {
namespace {

constexpr std::uint8_t kCmd0 = 0, kCmd8 = 8, kCmd9 = 9, kCmd12 = 12, kCmd13 = 13, kCmd16 = 16, kCmd17 = 17,
                       kCmd18 = 18, kCmd24 = 24, kCmd25 = 25, kCmd41 = 41, kCmd55 = 55, kCmd58 = 58;
constexpr std::uint8_t kTokenSingle = 0xFE, kTokenMultiWrite = 0xFC, kTokenStop = 0xFD;
constexpr std::uint8_t kR1Idle = 0x01, kR1IllegalCommand = 0x04;

constexpr std::uint32_t kInitTimeoutMs = 2000;   // ACMD41 may take up to a second
constexpr std::uint32_t kReadTokenTimeoutMs = 200;
constexpr std::uint32_t kBusyTimeoutMs = 750;    // spec: write programming up to 250 ms; allow for slow cards

} // namespace

bool SdCard::fail()
{
    release();
    ready_ = false;
    return false;
}

void SdCard::release()
{
    bus_.select(false);
    bus_.transfer(0xFF); // the card releases MISO on the next clock after CS goes high
}

bool SdCard::wait_not_busy(std::uint32_t timeout_ms)
{
    const std::uint32_t start = bus_.now_ms();
    do {
        if (bus_.transfer(0xFF) == 0xFF) return true;
    } while (bus_.now_ms() - start < timeout_ms);
    return false;
}

std::uint8_t SdCard::command(std::uint8_t index, std::uint32_t arg)
{
    bus_.select(true);
    // A card still programming holds MISO low; CMD0 is sent regardless (the card may not be
    // answering yet), anything else waits for it. CMD12 interrupts a multi-block read, during
    // which the card is sending data, not holding the line low: waiting would only time out.
    if (index != kCmd0 && index != kCmd12 && !wait_not_busy(kBusyTimeoutMs)) return 0xFF;

    const std::uint8_t crc = index == kCmd0 ? 0x95 : index == kCmd8 ? 0x87 : 0x01; // CRC is only checked for these two
    const std::uint8_t frame[6] = {static_cast<std::uint8_t>(0x40 | index), static_cast<std::uint8_t>(arg >> 24),
                                   static_cast<std::uint8_t>(arg >> 16), static_cast<std::uint8_t>(arg >> 8),
                                   static_cast<std::uint8_t>(arg), crc};
    bus_.write(frame, sizeof frame);
    if (index == kCmd12) bus_.transfer(0xFF); // the byte after CMD12 is a stuff byte

    for (int i = 0; i < 10; ++i) {  // R1 arrives within 8 bytes
        const std::uint8_t r = bus_.transfer(0xFF);
        if (!(r & 0x80)) return r;
    }
    return 0xFF;
}

std::uint8_t SdCard::app_command(std::uint8_t index, std::uint32_t arg)
{
    const std::uint8_t r = command(kCmd55, 0);
    if (r > kR1Idle) return r;
    return command(index, arg);
}

bool SdCard::init()
{
    ready_ = false;
    block_addressing_ = false;
    sectors_ = 0;

    bus_.set_clock_hz(init_hz_);
    bus_.select(false);
    for (int i = 0; i < 10; ++i) bus_.transfer(0xFF); // 80 clocks with the card deselected

    // CMD0: software reset into SPI mode (the card answers 0x01, "idle").
    bool idle = false;
    for (int attempt = 0; attempt < 10 && !idle; ++attempt) {
        idle = command(kCmd0, 0) == kR1Idle;
        release();
        if (!idle) bus_.delay_ms(10);
    }
    if (!idle) return fail();

    // CMD8: interface condition. A version 2 card echoes the voltage and check pattern; a
    // version 1 card (or MMC) rejects the command as illegal.
    bool v2 = false;
    const std::uint8_t r8 = command(kCmd8, 0x1AA);
    if (r8 & kR1IllegalCommand) {
        release();
    } else if (r8 == kR1Idle) {
        std::uint8_t r7[4];
        bus_.read(r7, 4);
        release();
        if ((r7[2] & 0x0F) != 0x01 || r7[3] != 0xAA) return fail(); // voltage range not accepted / wrong echo
        v2 = true;
    } else {
        return fail();
    }

    // ACMD41 until the card leaves idle. HCS tells a v2 card we can address SDHC/SDXC.
    const std::uint32_t start = bus_.now_ms();
    for (;;) {
        const std::uint8_t r = app_command(kCmd41, v2 ? 0x40000000u : 0);
        release();
        if (r == 0x00) break;
        if (r > kR1Idle || bus_.now_ms() - start > kInitTimeoutMs) return fail();
        bus_.delay_ms(5);
    }

    if (v2) {
        // CMD58: the OCR's CCS bit says block (SDHC/SDXC) or byte (SDSC) addressing.
        if (command(kCmd58, 0) != 0x00) return fail();
        std::uint8_t ocr[4];
        bus_.read(ocr, 4);
        release();
        block_addressing_ = (ocr[0] & 0x40) != 0;
    }
    if (!block_addressing_) {
        if (command(kCmd16, kSdSectorSize) != 0x00) return fail();
        release();
    }

    bus_.set_clock_hz(run_hz_);
    if (!read_csd()) return fail();
    ready_ = true;
    return true;
}

bool SdCard::read_data(std::uint8_t* dst, std::size_t len, std::uint32_t timeout_ms)
{
    // The card sends 0xFF until the data token, then the data and two CRC bytes.
    const std::uint32_t start = bus_.now_ms();
    std::uint8_t token;
    do {
        token = bus_.transfer(0xFF);
    } while (token == 0xFF && bus_.now_ms() - start < timeout_ms);
    if (token != kTokenSingle) return false; // timeout, or an error token
    bus_.read(dst, len);
    std::uint8_t crc[2];
    bus_.read(crc, 2); // not checked: the link is not CRC-protected in SPI mode by default
    return true;
}

bool SdCard::present()
{
    if (!ready_) return false;
    // R2 is R1 plus one status byte. Any R1 means a card answered; an absent card leaves MISO
    // high (pull-up), so command() times out with 0xFF.
    const std::uint8_t r1 = command(kCmd13, 0);
    if (r1 == 0xFF) return fail();
    bus_.transfer(0xFF);
    release();
    return true;
}

bool SdCard::read_csd()
{
    if (command(kCmd9, 0) != 0x00) return false;
    std::uint8_t csd[16];
    const bool got = read_data(csd, sizeof csd, kReadTokenTimeoutMs);
    release();
    if (!got) return false;

    if ((csd[0] >> 6) == 1) {
        // CSD v2 (SDHC/SDXC): capacity = (C_SIZE + 1) * 512 KiB.
        const std::uint32_t c_size = (static_cast<std::uint32_t>(csd[7] & 0x3F) << 16) |
                                     (static_cast<std::uint32_t>(csd[8]) << 8) | csd[9];
        sectors_ = (c_size + 1) * 1024;
    } else {
        // CSD v1: capacity = (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) * 2^READ_BL_LEN bytes.
        const unsigned read_bl_len = csd[5] & 0x0F;
        const std::uint32_t c_size = (static_cast<std::uint32_t>(csd[6] & 0x03) << 10) |
                                     (static_cast<std::uint32_t>(csd[7]) << 2) | (csd[8] >> 6);
        const unsigned c_size_mult = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
        const std::uint64_t bytes = static_cast<std::uint64_t>(c_size + 1) << (c_size_mult + 2 + read_bl_len);
        sectors_ = static_cast<std::uint32_t>(bytes / kSdSectorSize);
    }
    return sectors_ != 0;
}

bool SdCard::read(std::uint32_t lba, std::uint8_t* dst, std::uint32_t count)
{
    if (!ready_ || !dst) return false;
    if (count == 0) return true;
    if (lba >= sectors_ || count > sectors_ - lba) return false;

    if (count == 1) {
        if (command(kCmd17, address(lba)) != 0x00) return fail();
        if (!read_data(dst, kSdSectorSize, kReadTokenTimeoutMs)) return fail();
        release();
        return true;
    }
    if (command(kCmd18, address(lba)) != 0x00) return fail();
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!read_data(dst + static_cast<std::size_t>(i) * kSdSectorSize, kSdSectorSize, kReadTokenTimeoutMs)) return fail();
    }
    // CMD12 ends the transfer; the card may already be sending the next block, which it drops.
    if (command(kCmd12, 0) != 0x00 || !wait_not_busy(kBusyTimeoutMs)) return fail();
    release();
    return true;
}

bool SdCard::write_block(const std::uint8_t* src, std::uint8_t token)
{
    bus_.transfer(0xFF); // at least one byte between the command response and the data token
    bus_.transfer(token);
    bus_.write(src, kSdSectorSize);
    bus_.transfer(0xFF); // dummy CRC
    bus_.transfer(0xFF);
    return (bus_.transfer(0xFF) & 0x1F) == 0x05; // data accepted
}

bool SdCard::write(std::uint32_t lba, const std::uint8_t* src, std::uint32_t count)
{
    if (!ready_ || !src) return false;
    if (count == 0) return true;
    if (lba >= sectors_ || count > sectors_ - lba) return false;

    if (count == 1) {
        if (command(kCmd24, address(lba)) != 0x00) return fail();
        if (!write_block(src, kTokenSingle)) return fail();
        if (!wait_not_busy(kBusyTimeoutMs)) return fail();
        release();
        return true;
    }
    if (command(kCmd25, address(lba)) != 0x00) return fail();
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!wait_not_busy(kBusyTimeoutMs)) return fail();
        if (!write_block(src + static_cast<std::size_t>(i) * kSdSectorSize, kTokenMultiWrite)) return fail();
    }
    if (!wait_not_busy(kBusyTimeoutMs)) return fail();
    bus_.transfer(kTokenStop);
    bus_.transfer(0xFF);
    if (!wait_not_busy(kBusyTimeoutMs)) return fail();
    release();
    return true;
}

bool SdCard::sync()
{
    if (!ready_) return false;
    bus_.select(true);
    const bool idle = wait_not_busy(kBusyTimeoutMs);
    release();
    return idle ? true : fail();
}

} // namespace fujinet::platform::pico::detail
