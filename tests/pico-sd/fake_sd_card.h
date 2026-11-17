#pragma once
// A simulated SD card on an SPI bus, at the level of the SD Physical Layer spec's SPI mode:
// it enforces what a real card enforces (valid CRC on CMD0/CMD8, command framing, data tokens,
// busy signalling, byte vs block addressing) so the driver is checked against the protocol and
// not against its own assumptions. Virtual time advances with every byte clocked.
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "detail/sd_card.h"

namespace fakesd {

using fujinet::platform::pico::detail::SpiBus;

struct Options {
    bool sdhc = true;            // block addressing; false: SDSC, byte addressing
    bool v1 = false;             // SD v1: rejects CMD8 as illegal
    unsigned init_polls = 3;     // ACMD41 answers "idle" this many times first
    bool never_ready = false;    // ACMD41 never leaves idle
    bool absent = false;         // nothing answers: MISO reads 0xFF
    bool no_read_token = false;  // reads: the card never sends the data token
    bool reject_writes = false;  // writes: the card answers with a CRC-error data response
    bool stuck_busy = false;     // after a write the card never releases busy
    unsigned busy_bytes = 40;    // busy duration after a write, in byte times
    std::uint32_t sectors = 131072; // 64 MiB
};

class Card final : public SpiBus {
public:
    explicit Card(Options o = {}) : o_(o), image_(static_cast<std::size_t>(o.sectors) * 512, 0) {}

    // --- test access ---
    std::vector<std::uint8_t>& image() { return image_; }
    Options& options() { return o_; }
    unsigned long commands = 0, illegal = 0, cmd16 = 0;
    std::uint32_t max_clock_before_ready = 0, clock = 0;
    std::uint64_t time_ns = 0; // byte time at 10 MHz is 800 ns: microseconds would round it to nothing
    unsigned long reads = 0, writes = 0; // blocks transferred

    // --- SpiBus ---
    void set_clock_hz(std::uint32_t hz) override { clock = hz; }
    void select(bool on) override {
        if (selected_ && !on) abandon_frame();
        selected_ = on;
    }
    std::uint8_t transfer(std::uint8_t out) override {
        time_ns += 8000000000ull / (clock ? clock : 400000);
        if (!ready_ && clock > max_clock_before_ready) max_clock_before_ready = clock;
        if (o_.absent || !selected_) return 0xFF;
        return step(out);
    }
    void read(std::uint8_t* dst, std::size_t n) override { for (std::size_t i = 0; i < n; ++i) dst[i] = transfer(0xFF); }
    void write(const std::uint8_t* src, std::size_t n) override { for (std::size_t i = 0; i < n; ++i) transfer(src[i]); }
    std::uint32_t now_ms() override { return static_cast<std::uint32_t>(time_ns / 1000000); }
    void delay_ms(std::uint32_t ms) override { time_ns += static_cast<std::uint64_t>(ms) * 1000000; }

private:
    Options o_;
    std::vector<std::uint8_t> image_;
    bool stuck_ = false;
    bool selected_ = false, ready_ = false, idle_ = true, app_ = false;
    unsigned acmd41_calls_ = 0;
    std::deque<std::uint8_t> out_;           // bytes the card will clock out next
    std::vector<std::uint8_t> frame_;        // command being received
    enum class Phase { Command, WriteToken, WriteData } phase_ = Phase::Command;
    std::vector<std::uint8_t> data_;         // write data being received
    std::uint64_t write_addr_ = 0;
    bool multi_write_ = false;
    bool streaming_ = false;                 // CMD18 in progress
    std::uint64_t stream_addr_ = 0;

    void abandon_frame() { frame_.clear(); }

    std::uint64_t byte_addr(std::uint32_t arg) const { return o_.sdhc ? static_cast<std::uint64_t>(arg) * 512 : arg; }
    bool addr_ok(std::uint64_t a) const {
        if (!o_.sdhc && (a % 512)) return false;
        return a + 512 <= image_.size();
    }

    void queue_block(std::uint64_t addr) {
        if (o_.no_read_token || addr + 512 > image_.size()) return; // past the end a real card sends an error token
        out_.push_back(0xFE);
        for (int i = 0; i < 512; ++i) out_.push_back(image_[addr + i]);
        out_.push_back(0x12); out_.push_back(0x34); // CRC, ignored
        ++reads;
    }
    void respond(std::uint8_t r1) { out_.push_back(0xFF); out_.push_back(r1); } // R1 one byte late, as real cards may

    std::uint8_t step(std::uint8_t in) {
        if (stuck_) return 0x00; // never releases busy
        std::uint8_t ret = 0xFF;
        if (!out_.empty()) { ret = out_.front(); out_.pop_front(); }
        else if (streaming_) { queue_block(stream_addr_); stream_addr_ += 512; if (!out_.empty()) { ret = out_.front(); out_.pop_front(); } }

        switch (phase_) {
        case Phase::Command:
            if (frame_.empty()) { if ((in & 0xC0) == 0x40) frame_.push_back(in); }
            else { frame_.push_back(in); if (frame_.size() == 6) { auto f = frame_; frame_.clear(); command(f); } }
            break;
        case Phase::WriteToken:
            if (in == 0xFE || in == 0xFC) { data_.clear(); phase_ = Phase::WriteData; multi_write_ = in == 0xFC; }
            else if (in == 0xFD && multi_write_) { busy(); phase_ = Phase::Command; multi_write_ = false; }
            break;
        case Phase::WriteData:
            data_.push_back(in);
            if (data_.size() == 514) {
                if (o_.reject_writes) { out_.push_back(0x0B); phase_ = Phase::Command; multi_write_ = false; break; }
                if (!addr_ok(write_addr_)) { out_.push_back(0x0D); phase_ = Phase::Command; break; }
                std::memcpy(&image_[write_addr_], data_.data(), 512);
                ++writes;
                out_.push_back(0x05);
                busy();
                write_addr_ += 512;
                phase_ = multi_write_ ? Phase::WriteToken : Phase::Command;
            }
            break;
        }
        return ret;
    }
    void busy() { if (o_.stuck_busy) { stuck_ = true; return; } for (unsigned i = 0; i < o_.busy_bytes; ++i) out_.push_back(0x00); }

    void command(const std::vector<std::uint8_t>& f) {
        ++commands;
        const unsigned idx = f[0] & 0x3F;
        const std::uint32_t arg = (std::uint32_t{f[1]} << 24) | (f[2] << 16) | (f[3] << 8) | f[4];
        const bool was_app = app_;
        app_ = false;
        if (streaming_ && idx != 12) streaming_ = false;
        const std::uint8_t idle_bit = idle_ ? 0x01 : 0x00;

        switch (idx) {
        case 0:
            if (f[5] != 0x95) return; // CMD0 needs its CRC
            idle_ = true; ready_ = false; acmd41_calls_ = 0; respond(0x01); return;
        case 8:
            if (o_.v1) { ++illegal; respond(0x05); return; }
            if (f[5] != 0x87 || arg != 0x1AA) { ++illegal; respond(0x05); return; }
            respond(0x01); out_.insert(out_.end(), {0x00, 0x00, 0x01, 0xAA}); return;
        case 55: app_ = true; respond(idle_bit); return;
        case 13: respond(idle_bit); out_.push_back(0x00); return; // SEND_STATUS: R2 = R1 + one byte
        case 41:
            if (!was_app) { ++illegal; respond(0x05); return; }
            if (!o_.v1 && (arg & 0x40000000u) == 0 && o_.sdhc) { respond(0x01); return; } // SDHC wants HCS
            if (o_.never_ready || ++acmd41_calls_ <= o_.init_polls) { respond(0x01); return; }
            idle_ = false; ready_ = true; respond(0x00); return;
        case 58:
            if (idle_) { ++illegal; respond(0x05); return; }
            respond(0x00); out_.insert(out_.end(), {static_cast<std::uint8_t>(o_.sdhc ? 0xC0 : 0x80), 0xFF, 0x80, 0x00}); return;
        case 16:
            ++cmd16;
            if (arg != 512) { respond(0x40); return; }
            respond(0x00); return;
        case 9: {
            respond(0x00);
            std::uint8_t csd[16] = {};
            if (o_.sdhc) {
                csd[0] = 0x40; // CSD v2
                const std::uint32_t c_size = o_.sectors / 1024 - 1;
                csd[7] = (c_size >> 16) & 0x3F; csd[8] = (c_size >> 8) & 0xFF; csd[9] = c_size & 0xFF;
            } else {
                // CSD v1: READ_BL_LEN 9, C_SIZE_MULT 7 => (C_SIZE+1) * 512 * 512 bytes
                csd[5] = 9;
                const std::uint32_t c_size = o_.sectors / 512 - 1;
                csd[6] = (c_size >> 10) & 0x03; csd[7] = (c_size >> 2) & 0xFF; csd[8] = (c_size & 0x03) << 6;
                csd[9] = (7 >> 1) & 0x03; csd[10] = (7 & 1) << 7;
            }
            if (!o_.no_read_token) { out_.push_back(0xFE); out_.insert(out_.end(), csd, csd + 16); out_.push_back(0); out_.push_back(0); }
            return;
        }
        case 17: case 18: {
            if (idle_) { ++illegal; respond(0x05); return; }
            const auto a = byte_addr(arg);
            if (!addr_ok(a)) { respond(0x40); return; }
            respond(0x00);
            queue_block(a);
            if (idx == 18) { streaming_ = true; stream_addr_ = a + 512; }
            return;
        }
        case 12:
            out_.clear(); streaming_ = false;
            respond(0x00); busy(); return;
        case 24: case 25: {
            if (idle_) { ++illegal; respond(0x05); return; }
            const auto a = byte_addr(arg);
            if (!addr_ok(a)) { respond(0x40); return; }
            write_addr_ = a; multi_write_ = idx == 25; phase_ = Phase::WriteToken; respond(0x00); return;
        }
        default: ++illegal; respond(0x05); return;
        }
    }
};

} // namespace fakesd
