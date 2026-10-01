#pragma once

#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/packet_io.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace fujinet::io {

// The peer side of a packet link over a byte stream: docs/packet_link.md.
//
// NIO answers each request before it reads the next one (IOService handles a
// request and sends its response in the same pass). The link relies on that:
// a request still outstanding when receive() is next called was not answered,
// so the link completes it with Error::Unanswered.
class PacketLink : public IPacketIO {
public:
    enum class Kind : std::uint8_t { Packet = 0x01, Error = 0x02, Sync = 0x10, SyncAck = 0x11 };
    // Code 3 is reserved: it was Busy before Unanswered made it unreachable.
    enum class Error : std::uint8_t {
        Corrupt = 1,
        Oversized = 2,
        NotSynchronised = 4,
        Empty = 5,
        Unanswered = 6,
        UnsupportedVersion = 7,
    };

    static constexpr std::uint8_t kVersion = 1;
    static constexpr std::size_t kDefaultCapacity = 4096;
    static constexpr std::size_t kMinCapacity = 64;
    static constexpr std::size_t kMaxCapacity = 0xFFFF;
    // A byte stream from a microcontroller has no pauses inside a record;
    // TCP can stall for a retransmit (at least 200 ms) or a slow network.
    static constexpr std::uint32_t kDefaultRecordTimeoutMs = 100;
    static constexpr std::uint32_t kDefaultNetworkRecordTimeoutMs = 1000;
    static constexpr std::uint32_t kMaxRecordTimeoutMs = 60000;

    using Clock = std::function<std::uint64_t()>; // milliseconds

    // What a peer has seen since it started. For diagnostics only.
    struct Stats {
        std::uint32_t syncs{0};         // generations started
        std::uint32_t requests{0};      // requests handed to NIO
        std::uint32_t answers{0};       // responses sent
        std::uint32_t corrupt{0};       // records whose CRC failed
        std::uint32_t oversized{0};     // records longer than the capacity
        std::uint32_t abandoned{0};     // records that stopped arriving
        std::uint32_t unanswered{0};    // requests completed as Unanswered
        std::uint32_t refused{0};       // other Errors: NotSynchronised, Empty, UnsupportedVersion
    };

    explicit PacketLink(Channel& stream,
                        std::size_t capacity = kDefaultCapacity,
                        Clock now = {},
                        std::uint32_t recordTimeoutMs = kDefaultRecordTimeoutMs);

    PacketReceiveResult receive(std::uint8_t* buffer, std::size_t capacity) override;
    PacketIOStatus send(const std::uint8_t* packet, std::size_t size) override;
    PacketIOStatus reset() override;

    // Waits on the stream. A wait that lasts the record timeout with nothing
    // arriving is a real gap on the wire, so it ends a half-received record.
    // Time NIO spends busy elsewhere is not: the rest of a record may already
    // be waiting in the stream's buffer.
    bool wait_for_readable(std::chrono::milliseconds timeout);

    // Takes effect at once; controllers learn it from the next SyncAck.
    void set_record_timeout_ms(std::uint32_t ms);
    std::uint32_t record_timeout_ms() const { return _recordTimeoutMs; }

    std::string_view state_name() const;
    std::uint8_t generation() const { return _generation; }
    std::uint8_t version() const { return _state == State::Unsynchronised ? 0 : _version; }
    const Stats& stats() const { return _stats; }

    static std::vector<std::uint8_t> encode(Kind kind, std::uint8_t generation,
                                            const std::uint8_t* body, std::size_t size);
    static std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0);

private:
    enum class State { Unsynchronised, Idle, Busy };

    void fill();
    bool front_expired() const;
    void drop_front(std::size_t n);
    void abandon_front();
    void sync(std::uint8_t generation, const std::vector<std::uint8_t>& body);
    void reply(Kind kind, std::uint8_t generation, const std::uint8_t* body = nullptr, std::size_t size = 0);
    void reply_error(std::uint8_t generation, Error e);

    Channel& _stream;
    Clock _now;
    std::uint32_t _recordTimeoutMs;
    std::vector<std::uint8_t> _rx;
    std::uint64_t _lastByteMs{0};
    // The record at the front of _rx was incomplete when receive() last
    // returned, and NIO then waited out a gap of the record timeout.
    bool _frontIncomplete{false};
    bool _frontStale{false};
    State _state{State::Unsynchronised};
    std::uint8_t _generation{0};
    std::uint8_t _version{0};
    Stats _stats{};
};

// Packet link settings derived from configuration (config::PacketLinkConfig):
// zero means the default, and values are kept within the link's limits.
struct PacketLinkSettings {
    std::size_t capacity{PacketLink::kDefaultCapacity};
    std::uint32_t recordTimeoutMs{PacketLink::kDefaultRecordTimeoutMs};
};

// `networked` is a stream that can stall in transit, such as TCP.
PacketLinkSettings packet_link_settings(std::uint32_t capacity, std::uint32_t recordTimeoutMs, bool networked);

// A byte channel that carries a packet link: packet_io() is the link, and the
// byte interface is unused.
class PacketLinkChannel : public Channel {
public:
    explicit PacketLinkChannel(std::unique_ptr<Channel> stream,
                               std::size_t capacity = PacketLink::kDefaultCapacity);
    PacketLinkChannel(std::unique_ptr<Channel> stream, const PacketLinkSettings& settings);

    IPacketIO* packet_io() override { return &_link; }
    bool available() override { return false; }
    std::size_t read(std::uint8_t*, std::size_t) override { return 0; }
    void write(const std::uint8_t*, std::size_t) override {}
    bool supports_readable_wait() const override { return _stream->supports_readable_wait(); }
    bool wait_for_readable(std::chrono::milliseconds timeout) override
    {
        return _link.wait_for_readable(timeout);
    }

    PacketLink& link() { return _link; }
    // The byte stream underneath, e.g. for its own diagnostics.
    Channel& stream() { return *_stream; }

private:
    std::unique_ptr<Channel> _stream;
    PacketLink _link;
};

// `stream` wrapped in a PacketLinkChannel when `packet_link` is set (a build
// profile's packetLink), otherwise `stream` itself.
std::unique_ptr<Channel> with_packet_link(bool packet_link, std::unique_ptr<Channel> stream,
                                          const PacketLinkSettings& settings = {});

} // namespace fujinet::io
