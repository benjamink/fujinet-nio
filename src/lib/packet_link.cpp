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

std::uint32_t clamp_record_timeout(std::uint32_t ms)
{
    return std::clamp<std::uint32_t>(ms, 1, PacketLink::kMaxRecordTimeoutMs);
}

} // namespace

PacketLink::PacketLink(Channel& stream, std::size_t capacity, Clock now, std::uint32_t recordTimeoutMs)
    : IPacketIO(capacity)
    , _stream(stream)
    , _now(now ? std::move(now) : Clock(steady_ms))
    , _recordTimeoutMs(clamp_record_timeout(recordTimeoutMs))
{
    _rx.reserve(kHeader + capacity + kCrc);
}

void PacketLink::set_record_timeout_ms(std::uint32_t ms)
{
    _recordTimeoutMs = clamp_record_timeout(ms);
}

std::string_view PacketLink::state_name() const
{
    switch (_state) {
    case State::Idle: return "idle";
    case State::Busy: return "request outstanding";
    case State::Unsynchronised:
    default: return "unsynchronised";
    }
}

bool PacketLink::wait_for_readable(std::chrono::milliseconds timeout)
{
    const std::uint64_t started = _now();
    const bool ready = _stream.wait_for_readable(timeout);
    if (_frontIncomplete && _now() - started >= _recordTimeoutMs) _frontStale = true;
    return ready;
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
        // Reading late after a busy spell refreshes this too, so time spent
        // busy is never mistaken for a gap (see wait_for_readable).
        _lastByteMs = _now();
    }
}

bool PacketLink::front_expired() const
{
    return _frontStale || _now() - _lastByteMs >= _recordTimeoutMs;
}

void PacketLink::drop_front(std::size_t n)
{
    _rx.erase(_rx.begin(), _rx.begin() + static_cast<std::ptrdiff_t>(std::min(n, _rx.size())));
    _frontStale = false;
}

void PacketLink::abandon_front()
{
    ++_stats.abandoned;
    drop_front(2);
}

void PacketLink::sync(std::uint8_t generation, const std::vector<std::uint8_t>& body)
{
    // Whatever the outcome, the controller has abandoned the old generation.
    _state = State::Unsynchronised;
    if (body.empty() || body[0] == 0) {
        ++_stats.refused;
        reply_error(generation, Error::UnsupportedVersion);
        return;
    }
    _generation = generation;
    _version = std::min(body[0], kVersion);
    _state = State::Idle;
    ++_stats.syncs;
    const std::size_t cap = std::min(capacity(), kMaxCapacity);
    const std::uint8_t ack[] = {
        _version,
        static_cast<std::uint8_t>(cap),
        static_cast<std::uint8_t>(cap >> 8),
        static_cast<std::uint8_t>(_recordTimeoutMs),
        static_cast<std::uint8_t>(_recordTimeoutMs >> 8),
    };
    reply(Kind::SyncAck, generation, ack, sizeof(ack));
}

PacketReceiveResult PacketLink::receive(std::uint8_t* buffer, std::size_t limit)
{
    if (_state == State::Busy) {
        // NIO reads again only after answering (see the class comment), so this
        // request got no answer: it was not a valid FujiBus packet, or its answer
        // could not be sent. It may have run.
        ++_stats.unanswered;
        reply_error(_generation, Error::Unanswered);
        _state = State::Idle;
    }
    _frontIncomplete = false;
    for (;;) {
        fill();
        auto start = _rx.begin();
        while (start != _rx.end() && !(*start == kSync0 && (start + 1 == _rx.end() || *(start + 1) == kSync1))) {
            ++start;
        }
        if (start != _rx.begin()) drop_front(static_cast<std::size_t>(start - _rx.begin()));
        if (_rx.size() < kHeader) {
            if (_stream.available()) continue;
            if (_rx.empty()) return {PacketIOStatus::NoData};
            if (front_expired()) {
                abandon_front();
                continue;
            }
            _frontIncomplete = true;
            return {PacketIOStatus::Incomplete};
        }

        const auto kind = static_cast<Kind>(_rx[2]);
        const std::uint8_t generation = _rx[3];
        const std::size_t size = _rx[4] | (static_cast<std::size_t>(_rx[5]) << 8);
        if (size > capacity()) {
            // The length may be corrupt, so rescan rather than skip the body.
            ++_stats.oversized;
            if (kind == Kind::Packet && _state == State::Idle && generation == _generation) {
                reply_error(generation, Error::Oversized);
            }
            drop_front(2);
            continue;
        }
        const std::size_t total = kHeader + size + kCrc;
        if (_rx.size() < total) {
            if (front_expired()) {
                abandon_front();
                continue;
            }
            _frontIncomplete = true;
            return {PacketIOStatus::Incomplete};
        }

        std::uint32_t crc = 0;
        for (std::size_t i = 0; i < kCrc; ++i) crc |= static_cast<std::uint32_t>(_rx[kHeader + size + i]) << (8 * i);
        if (crc != crc32(_rx.data() + 2, kHeader - 2 + size)) {
            ++_stats.corrupt;
            if (kind == Kind::Packet && _state == State::Idle && generation == _generation) {
                reply_error(generation, Error::Corrupt);
            }
            drop_front(2);
            continue;
        }

        std::vector<std::uint8_t> body(_rx.begin() + kHeader, _rx.begin() + kHeader + size);
        drop_front(total);

        switch (kind) {
        case Kind::Sync:
            sync(generation, body);
            break;
        case Kind::Packet:
            // Never Busy here: an outstanding request was completed on entry.
            if (_state == State::Unsynchronised || generation != _generation) {
                ++_stats.refused;
                reply_error(generation, Error::NotSynchronised);
            } else if (body.empty()) {
                ++_stats.refused;
                reply_error(generation, Error::Empty);
            } else if (body.size() > limit) {
                ++_stats.oversized;
                reply_error(generation, Error::Oversized);
            } else {
                std::copy(body.begin(), body.end(), buffer);
                _state = State::Busy;
                ++_stats.requests;
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
    ++_stats.answers;
    return PacketIOStatus::Ok;
}

PacketIOStatus PacketLink::reset()
{
    _rx.clear();
    _frontIncomplete = false;
    _frontStale = false;
    _state = State::Unsynchronised;
    return PacketIOStatus::Ok;
}

PacketLinkSettings packet_link_settings(std::uint32_t capacity, std::uint32_t recordTimeoutMs, bool networked)
{
    PacketLinkSettings s{};
    if (capacity != 0) {
        s.capacity = std::clamp<std::size_t>(capacity, PacketLink::kMinCapacity, PacketLink::kMaxCapacity);
    }
    s.recordTimeoutMs = recordTimeoutMs != 0 ? clamp_record_timeout(recordTimeoutMs)
                        : networked          ? PacketLink::kDefaultNetworkRecordTimeoutMs
                                             : PacketLink::kDefaultRecordTimeoutMs;
    return s;
}

PacketLinkChannel::PacketLinkChannel(std::unique_ptr<Channel> stream, std::size_t capacity)
    : _stream(std::move(stream)), _link(*_stream, capacity)
{
}

PacketLinkChannel::PacketLinkChannel(std::unique_ptr<Channel> stream, const PacketLinkSettings& settings)
    : _stream(std::move(stream)), _link(*_stream, settings.capacity, {}, settings.recordTimeoutMs)
{
}

std::unique_ptr<Channel> with_packet_link(bool packet_link, std::unique_ptr<Channel> stream,
                                          const PacketLinkSettings& settings)
{
    if (!packet_link || !stream) return stream;
    return std::make_unique<PacketLinkChannel>(std::move(stream), settings);
}

} // namespace fujinet::io
