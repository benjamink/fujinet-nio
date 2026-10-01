#pragma once

#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/packet_io.h"

#include <cstdint>
#include <functional>
#include <memory>
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
    static constexpr std::uint64_t kRecordTimeoutMs = 100;

    using Clock = std::function<std::uint64_t()>; // milliseconds

    explicit PacketLink(Channel& stream, std::size_t capacity = kDefaultCapacity, Clock now = {});

    PacketReceiveResult receive(std::uint8_t* buffer, std::size_t capacity) override;
    PacketIOStatus send(const std::uint8_t* packet, std::size_t size) override;
    PacketIOStatus reset() override;

    static std::vector<std::uint8_t> encode(Kind kind, std::uint8_t generation,
                                            const std::uint8_t* body, std::size_t size);
    static std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0);

private:
    enum class State { Unsynchronised, Idle, Busy };

    void fill();
    bool front_expired() const;
    void drop_front(std::size_t n);
    void sync(std::uint8_t generation, const std::vector<std::uint8_t>& body);
    void reply(Kind kind, std::uint8_t generation, const std::uint8_t* body = nullptr, std::size_t size = 0);
    void reply_error(std::uint8_t generation, Error e);

    Channel& _stream;
    Clock _now;
    std::vector<std::uint8_t> _rx;
    std::uint64_t _lastByteMs{0};
    // The record at the front of _rx was incomplete when the last receive()
    // returned, and its bytes then stopped for kRecordTimeoutMs.
    bool _frontIncomplete{false};
    bool _frontStale{false};
    State _state{State::Unsynchronised};
    std::uint8_t _generation{0};
};

// A byte channel that carries a packet link: packet_io() is the link, and the
// byte interface is unused.
class PacketLinkChannel : public Channel {
public:
    explicit PacketLinkChannel(std::unique_ptr<Channel> stream,
                               std::size_t capacity = PacketLink::kDefaultCapacity);

    IPacketIO* packet_io() override { return &_link; }
    bool available() override { return false; }
    std::size_t read(std::uint8_t*, std::size_t) override { return 0; }
    void write(const std::uint8_t*, std::size_t) override {}
    bool supports_readable_wait() const override { return _stream->supports_readable_wait(); }
    bool wait_for_readable(std::chrono::milliseconds timeout) override
    {
        return _stream->wait_for_readable(timeout);
    }

private:
    std::unique_ptr<Channel> _stream;
    PacketLink _link;
};

// `stream` wrapped in a PacketLinkChannel when `packet_link` is set (a build
// profile's packetLink), otherwise `stream` itself.
std::unique_ptr<Channel> with_packet_link(bool packet_link, std::unique_ptr<Channel> stream);

} // namespace fujinet::io
