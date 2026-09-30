#include "fujinet/io/core/packet_link.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace fujinet::io {

namespace {

constexpr std::uint8_t kSync0 = 0xF5;
constexpr std::uint8_t kSync1 = 0x4E;
constexpr std::size_t kHeader = 6;
constexpr std::size_t kCrc = 4;

std::uint64_t steady_ms()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

PacketLink::PacketLink(Channel& stream, std::size_t capacity, Clock now)
    : IPacketIO(capacity), _stream(stream), _now(now ? std::move(now) : Clock(steady_ms))
{
    _rx.reserve(kHeader + capacity + kCrc);
}

std::uint32_t PacketLink::crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc)
{
    static constexpr std::uint32_t nibble[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        crc = (crc >> 4) ^ nibble[crc & 0x0F];
        crc = (crc >> 4) ^ nibble[crc & 0x0F];
    }
    return ~crc;
}

std::vector<std::uint8_t> PacketLink::encode(Kind kind, std::uint8_t generation,
                                             const std::uint8_t* body, std::size_t size)
{
    std::vector<std::uint8_t> out(kHeader + size + kCrc);
    out[0] = kSync0;
    out[1] = kSync1;
    out[2] = static_cast<std::uint8_t>(kind);
    out[3] = generation;
    out[4] = static_cast<std::uint8_t>(size);
    out[5] = static_cast<std::uint8_t>(size >> 8);
    if (size) std::memcpy(out.data() + kHeader, body, size);
    const std::uint32_t crc = crc32(out.data() + 2, kHeader - 2 + size);
    for (std::size_t i = 0; i < kCrc; ++i) out[kHeader + size + i] = static_cast<std::uint8_t>(crc >> (8 * i));
    return out;
}

void PacketLink::reply(Kind kind, std::uint8_t generation, const std::uint8_t* body, std::size_t size)
{
    const auto record = encode(kind, generation, body, size);
    _stream.write(record.data(), record.size());
}

void PacketLink::reply_error(std::uint8_t generation, Error e)
{
    const auto code = static_cast<std::uint8_t>(e);
    reply(Kind::Error, generation, &code, 1);
}

void PacketLink::fill()
{
    const std::size_t limit = kHeader + capacity() + kCrc;
    std::uint8_t chunk[256];
    while (_rx.size() < limit && _stream.available()) {
        const std::size_t n = _stream.read(chunk, std::min(sizeof(chunk), limit - _rx.size()));
        if (n == 0) break;
        _rx.insert(_rx.end(), chunk, chunk + n);
        _lastByteMs = _now();
    }
}

PacketReceiveResult PacketLink::receive(std::uint8_t* buffer, std::size_t limit)
{
    for (;;) {
        fill();
        auto sync = _rx.begin();
        while (sync != _rx.end() && !(*sync == kSync0 && (sync + 1 == _rx.end() || *(sync + 1) == kSync1))) {
            ++sync;
        }
        _rx.erase(_rx.begin(), sync);
        if (_rx.size() < kHeader) {
            if (_stream.available()) continue;
            return {_rx.empty() ? PacketIOStatus::NoData : PacketIOStatus::Incomplete};
        }

        const auto kind = static_cast<Kind>(_rx[2]);
        const std::uint8_t generation = _rx[3];
        const std::size_t size = _rx[4] | (static_cast<std::size_t>(_rx[5]) << 8);
        if (size > capacity()) {
            // The length may be corrupt, so rescan rather than skip the body.
            if (kind == Kind::Packet && _state == State::Idle && generation == _generation) {
                reply_error(generation, Error::Oversized);
            }
            _rx.erase(_rx.begin(), _rx.begin() + 2);
            continue;
        }
        const std::size_t total = kHeader + size + kCrc;
        if (_rx.size() < total) {
            if (_now() - _lastByteMs < kRecordTimeoutMs) return {PacketIOStatus::Incomplete};
            _rx.erase(_rx.begin(), _rx.begin() + 2);
            continue;
        }

        std::uint32_t crc = 0;
        for (std::size_t i = 0; i < kCrc; ++i) crc |= static_cast<std::uint32_t>(_rx[kHeader + size + i]) << (8 * i);
        if (crc != crc32(_rx.data() + 2, kHeader - 2 + size)) {
            if (kind == Kind::Packet && _state == State::Idle && generation == _generation) {
                reply_error(generation, Error::Corrupt);
            }
            _rx.erase(_rx.begin(), _rx.begin() + 2);
            continue;
        }

        std::vector<std::uint8_t> body(_rx.begin() + kHeader, _rx.begin() + kHeader + size);
        _rx.erase(_rx.begin(), _rx.begin() + total);

        switch (kind) {
        case Kind::Sync:
            _generation = generation;
            _state = State::Idle;
            reply(Kind::SyncAck, generation);
            break;
        case Kind::Packet:
            if (_state == State::Unsynchronised || generation != _generation) {
                reply_error(generation, Error::NotSynchronised);
            } else if (_state == State::Busy) {
                reply_error(generation, Error::Busy);
            } else if (body.empty()) {
                reply_error(generation, Error::Empty);
            } else if (body.size() > limit) {
                reply_error(generation, Error::Oversized);
            } else {
                std::copy(body.begin(), body.end(), buffer);
                _state = State::Busy;
                return {PacketIOStatus::Ok, body.size()};
            }
            break;
        default:
            break;
        }
    }
}

PacketIOStatus PacketLink::send(const std::uint8_t* packet, std::size_t size)
{
    if (size == 0) return PacketIOStatus::EmptyPacket;
    if (size > capacity()) return PacketIOStatus::Oversized;
    // Only the outstanding request of the current generation gets an answer.
    if (_state != State::Busy) return PacketIOStatus::SendFailed;
    reply(Kind::Packet, _generation, packet, size);
    _state = State::Idle;
    return PacketIOStatus::Ok;
}

PacketIOStatus PacketLink::reset()
{
    _rx.clear();
    _state = State::Unsynchronised;
    return PacketIOStatus::Ok;
}

PacketLinkChannel::PacketLinkChannel(std::unique_ptr<Channel> stream, std::size_t capacity)
    : _stream(std::move(stream)), _link(*_stream, capacity)
{
}

} // namespace fujinet::io
