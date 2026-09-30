#pragma once

#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/packet_io.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace fujinet::io {

// The peer side of a packet link over a byte stream: docs/packet_link.md.
class PacketLink : public IPacketIO {
public:
    enum class Kind : std::uint8_t { Packet = 0x01, Error = 0x02, Sync = 0x10, SyncAck = 0x11 };
    enum class Error : std::uint8_t { Corrupt = 1, Oversized = 2, Busy = 3, NotSynchronised = 4, Empty = 5 };

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
    void reply(Kind kind, std::uint8_t generation, const std::uint8_t* body = nullptr, std::size_t size = 0);
    void reply_error(std::uint8_t generation, Error e);

    Channel& _stream;
    Clock _now;
    std::vector<std::uint8_t> _rx;
    std::uint64_t _lastByteMs{0};
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

} // namespace fujinet::io
