#include "doctest.h"

#include "fujibus_wire_fixtures.h"

#include "fujinet/build/profile.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/core/packet_link.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

using fujinet::io::Channel;
using fujinet::io::PacketIOStatus;
using fujinet::io::PacketLink;
using fujinet::io::PacketLinkChannel;
using Bytes = std::vector<std::uint8_t>;
using Kind = PacketLink::Kind;
using Error = PacketLink::Error;

namespace {

class Stream : public Channel {
public:
    void push(const Bytes& b) { _rx.insert(_rx.end(), b.begin(), b.end()); }
    Bytes take()
    {
        Bytes out(_tx.begin(), _tx.end());
        _tx.clear();
        return out;
    }
    bool available() override { return !_rx.empty(); }
    std::size_t read(std::uint8_t* buf, std::size_t max) override
    {
        std::size_t n = 0;
        while (n < max && !_rx.empty()) {
            buf[n++] = _rx.front();
            _rx.pop_front();
        }
        return n;
    }
    void write(const std::uint8_t* buf, std::size_t len) override { _tx.insert(_tx.end(), buf, buf + len); }

private:
    std::deque<std::uint8_t> _rx;
    std::deque<std::uint8_t> _tx;
};

Bytes record(Kind kind, std::uint8_t gen, const Bytes& body = {})
{
    return PacketLink::encode(kind, gen, body.data(), body.size());
}

Bytes error(std::uint8_t gen, Error e)
{
    return record(Kind::Error, gen, {static_cast<std::uint8_t>(e)});
}

Bytes sync_record(std::uint8_t gen, std::uint8_t version = PacketLink::kVersion)
{
    return record(Kind::Sync, gen, {version});
}

// SyncAck from a peer of capacity 64.
Bytes sync_ack(std::uint8_t gen)
{
    return record(Kind::SyncAck, gen, {PacketLink::kVersion, 64, 0});
}

struct Link {
    Stream stream;
    std::uint64_t now = 0;
    PacketLink link{stream, 64, [this] { return now; }};
    std::uint8_t buf[64];

    PacketIOStatus receive() { return link.receive(buf, sizeof(buf)).status; }

    void sync(std::uint8_t gen)
    {
        stream.push(sync_record(gen));
        CHECK(receive() == PacketIOStatus::NoData);
        CHECK(stream.take() == sync_ack(gen));
    }
};

// Answers every request with a payload of `size` bytes.
class SizedReplyDevice : public fujinet::io::VirtualDevice {
public:
    explicit SizedReplyDevice(std::size_t size) : _size(size) {}
    fujinet::io::IOResponse handle(const fujinet::io::IORequest& req) override
    {
        fujinet::io::IOResponse resp;
        resp.id = req.id;
        resp.deviceId = req.deviceId;
        resp.status = fujinet::io::StatusCode::Ok;
        resp.command = req.command;
        resp.payload.assign(_size, 0x5A);
        return resp;
    }
    void poll() override {}

private:
    std::size_t _size;
};

constexpr std::uint8_t kTestDevice = 0x70;

Bytes request_for(std::uint8_t device, std::uint8_t command)
{
    return fujinet::io::protocol::FujiBusPacket(static_cast<fujinet::io::protocol::WireDeviceId>(device), command)
        .serializeRaw();
}

// One record decoded from the peer's output; the CRC must be valid.
struct Decoded {
    Kind kind;
    std::uint8_t generation;
    Bytes body;
};

std::vector<Decoded> decode_all(const Bytes& out)
{
    std::vector<Decoded> records;
    std::size_t at = 0;
    while (at < out.size()) {
        REQUIRE(out.size() - at >= 10);
        REQUIRE(out[at] == 0xF5);
        REQUIRE(out[at + 1] == 0x4E);
        const std::size_t size = out[at + 4] | (static_cast<std::size_t>(out[at + 5]) << 8);
        REQUIRE(out.size() - at >= 10 + size);
        std::uint32_t crc = 0;
        for (std::size_t i = 0; i < 4; ++i) crc |= static_cast<std::uint32_t>(out[at + 6 + size + i]) << (8 * i);
        REQUIRE(crc == PacketLink::crc32(out.data() + at + 2, 4 + size));
        records.push_back({static_cast<Kind>(out[at + 2]), out[at + 3],
                           Bytes(out.begin() + static_cast<std::ptrdiff_t>(at + 6),
                                 out.begin() + static_cast<std::ptrdiff_t>(at + 6 + size))});
        at += 10 + size;
    }
    return records;
}

// NIO's core behind a packet link of `capacity`, with SizedReplyDevice at kTestDevice.
struct LinkedCore {
    explicit LinkedCore(std::size_t capacity = PacketLink::kDefaultCapacity, std::size_t reply = 8)
    {
        auto owned = std::make_unique<Stream>();
        stream = owned.get();
        channel = std::make_unique<PacketLinkChannel>(std::move(owned), capacity);
        fujinet::build::BuildProfile profile{};
        profile.primaryTransport = fujinet::build::TransportKind::FujiBusNative;
        REQUIRE(fujinet::core::setup_transports(core, *channel, profile) != nullptr);
        REQUIRE(core.deviceManager().registerDevice(kTestDevice, std::make_unique<SizedReplyDevice>(reply)));
    }

    std::vector<Decoded> exchange(const Bytes& records)
    {
        stream->push(records);
        for (int i = 0; i < 4; ++i) core.tick();
        return decode_all(stream->take());
    }

    Stream* stream{nullptr};
    std::unique_ptr<PacketLinkChannel> channel;
    fujinet::core::FujinetCore core;
};

} // namespace

TEST_CASE("PacketLink CRC is CRC-32/IEEE")
{
    const Bytes check{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(PacketLink::crc32(check.data(), check.size()) == 0xCBF43926);
}

TEST_CASE("PacketLink records match the Python controller's")
{
    // The same records are checked in py/tests/test_packetlink.py.
    CHECK(sync_record(7) == Bytes{0xF5, 0x4E, 0x10, 0x07, 0x01, 0x00, 0x01, 0x87, 0x02, 0xD0, 0x4D});
    CHECK(record(Kind::SyncAck, 7, {0x01, 0x00, 0x10}) ==
          Bytes{0xF5, 0x4E, 0x11, 0x07, 0x03, 0x00, 0x01, 0x00, 0x10, 0x3F, 0x78, 0x15, 0x1A});
    CHECK(record(Kind::Packet, 7, fujibus_wire_fixtures::minimum.raw) ==
          Bytes{0xF5, 0x4E, 0x01, 0x07, 0x06, 0x00, 0x01, 0x02, 0x06, 0x00, 0x09, 0x00, 0x56, 0x03, 0x73, 0xEB});
}

TEST_CASE("PacketLink answers a request in the controller's generation")
{
    Link l;
    l.sync(7);
    // Sync bytes, SLIP bytes and zeros are all ordinary body bytes.
    const Bytes request{0x03, 0x04, 0x0C, 0x00, 0x00, 0xF5, 0x4E, 0xC0, 0xDB, 0x00, 0xF5, 0x4E};
    l.stream.push(record(Kind::Packet, 7, request));
    const auto r = l.link.receive(l.buf, sizeof(l.buf));
    REQUIRE(r.status == PacketIOStatus::Ok);
    CHECK(Bytes(l.buf, l.buf + r.size) == request);

    const Bytes response{0xAA, 0xBB};
    CHECK(l.link.send(response.data(), response.size()) == PacketIOStatus::Ok);
    CHECK(l.stream.take() == record(Kind::Packet, 7, response));
    // Nothing is outstanding now, so another answer has nothing to answer.
    CHECK(l.link.send(response.data(), response.size()) == PacketIOStatus::SendFailed);
    CHECK(l.stream.take().empty());
}

TEST_CASE("PacketLink rejects requests outside a generation")
{
    Link l;
    l.stream.push(record(Kind::Packet, 1, {1, 2, 3}));
    CHECK(l.receive() == PacketIOStatus::NoData);
    CHECK(l.stream.take() == error(1, Error::NotSynchronised));

    l.sync(2);
    l.stream.push(record(Kind::Packet, 3, {1, 2, 3}));
    CHECK(l.receive() == PacketIOStatus::NoData);
    CHECK(l.stream.take() == error(3, Error::NotSynchronised));
}

TEST_CASE("PacketLink Sync and SyncAck carry the link version and the peer's capacity")
{
    Link l;
    l.stream.push(sync_record(3, 1));
    CHECK(l.receive() == PacketIOStatus::NoData);
    CHECK(l.stream.take() == sync_ack(3));

    // A controller that speaks a later version is answered with this one.
    l.stream.push(sync_record(4, 9));
    CHECK(l.receive() == PacketIOStatus::NoData);
    CHECK(l.stream.take() == record(Kind::SyncAck, 4, {1, 64, 0}));
}

TEST_CASE("PacketLink refuses a Sync without a version it speaks")
{
    for (const Bytes& body : {Bytes{}, Bytes{0}}) {
        Link l;
        l.sync(1);
        l.stream.push(record(Kind::Sync, 2, body));
        CHECK(l.receive() == PacketIOStatus::NoData);
        CHECK(l.stream.take() == error(2, Error::UnsupportedVersion));
        // The old generation is gone too: the controller abandoned it.
        l.stream.push(record(Kind::Packet, 1, {1}));
        CHECK(l.receive() == PacketIOStatus::NoData);
        CHECK(l.stream.take() == error(1, Error::NotSynchronised));
    }
}

TEST_CASE("PacketLink completes a request NIO did not answer before reading the next")
{
    Link l;
    l.sync(1);
    l.stream.push(record(Kind::Packet, 1, {1}));
    CHECK(l.receive() == PacketIOStatus::Ok);
    // NIO reads again without answering: the request is completed as Unanswered.
    l.stream.push(record(Kind::Packet, 1, {2}));
    CHECK(l.receive() == PacketIOStatus::Ok);
    CHECK(l.stream.take() == error(1, Error::Unanswered));
    CHECK(l.buf[0] == 2);
}

TEST_CASE("PacketLink capacity: the maximum is accepted and one more is an error")
{
    Link l;
    l.sync(1);
    l.stream.push(record(Kind::Packet, 1, Bytes(65, 0x11)));
    l.stream.push(record(Kind::Packet, 1, Bytes(64, 0x22)));
    const auto r = l.link.receive(l.buf, sizeof(l.buf));
    CHECK(l.stream.take() == error(1, Error::Oversized));
    REQUIRE(r.status == PacketIOStatus::Ok);
    CHECK(r.size == 64);
    CHECK(l.buf[63] == 0x22);
}

TEST_CASE("PacketLink reports a corrupt request and the next one still arrives")
{
    Link l;
    l.sync(1);
    auto bad = record(Kind::Packet, 1, {1, 2, 3});
    bad[7] ^= 0xFF;
    l.stream.push(bad);
    l.stream.push(record(Kind::Packet, 1, {4, 5, 6}));
    const auto r = l.link.receive(l.buf, sizeof(l.buf));
    CHECK(l.stream.take() == error(1, Error::Corrupt));
    REQUIRE(r.status == PacketIOStatus::Ok);
    CHECK(Bytes(l.buf, l.buf + r.size) == Bytes{4, 5, 6});
}

TEST_CASE("PacketLink ignores noise and reassembles a record that arrives in pieces")
{
    Link l;
    l.stream.push({0x00, 0xF5, 0x12, 0x4E, 0xF5});
    CHECK(l.receive() == PacketIOStatus::Incomplete);
    const auto sync = sync_record(4);
    l.stream.push(Bytes(sync.begin() + 1, sync.end()));
    CHECK(l.receive() == PacketIOStatus::NoData);
    CHECK(l.stream.take() == sync_ack(4));

    const auto req = record(Kind::Packet, 4, {9, 8, 7});
    for (std::size_t i = 0; i + 1 < req.size(); ++i) {
        l.stream.push({req[i]});
        CHECK(l.receive() == PacketIOStatus::Incomplete);
    }
    l.stream.push({req.back()});
    CHECK(l.receive() == PacketIOStatus::Ok);
}

TEST_CASE("PacketLink abandons a record that stops arriving")
{
    Link l;
    l.sync(1);
    const auto req = record(Kind::Packet, 1, {1, 2, 3});
    l.stream.push(Bytes(req.begin(), req.end() - 2));
    CHECK(l.receive() == PacketIOStatus::Incomplete);
    l.now += PacketLink::kRecordTimeoutMs;
    CHECK(l.receive() == PacketIOStatus::NoData);
    l.stream.push(record(Kind::Packet, 1, {4}));
    CHECK(l.receive() == PacketIOStatus::Ok);
    CHECK(l.buf[0] == 4);
}

TEST_CASE("PacketLink completes the outstanding request on resync and drops a late answer")
{
    Link l;
    l.sync(1);
    l.stream.push(record(Kind::Packet, 1, {1}));
    CHECK(l.receive() == PacketIOStatus::Ok);
    l.stream.push(sync_record(2));
    CHECK(l.receive() == PacketIOStatus::NoData);
    Bytes expected = error(1, Error::Unanswered);
    const Bytes ack = sync_ack(2);
    expected.insert(expected.end(), ack.begin(), ack.end());
    CHECK(l.stream.take() == expected);
    const Bytes response{0xAA};
    CHECK(l.link.send(response.data(), response.size()) == PacketIOStatus::SendFailed);
    CHECK(l.stream.take().empty());
}

TEST_CASE("PacketLink: a quiet gap before Sync recovers from a cut-off record")
{
    // A record cut off after its sync bytes would read its length from the
    // next record's header. The controller's quiet pause ends it, whether or
    // not NIO polled during the pause.
    for (const bool polled : {true, false}) {
        CAPTURE(polled);
        Link l;
        l.stream.push({0xF5, 0x4E});
        CHECK(l.receive() == PacketIOStatus::Incomplete);
        l.now += PacketLink::kRecordTimeoutMs;
        if (polled) CHECK(l.receive() == PacketIOStatus::NoData);
        l.stream.push(sync_record(5));
        CHECK(l.receive() == PacketIOStatus::NoData);
        CHECK(l.stream.take() == sync_ack(5));
    }
}

TEST_CASE("PacketLink keeps a record that arrives slowly but steadily")
{
    // The timeout is a gap between bytes, not a limit on the whole record, so a
    // large record on a slow UART still arrives.
    Link l;
    l.sync(1);
    const auto req = record(Kind::Packet, 1, Bytes(60, 0x33));
    for (std::size_t i = 0; i + 1 < req.size(); ++i) {
        l.stream.push({req[i]});
        CHECK(l.receive() == PacketIOStatus::Incomplete);
        l.now += PacketLink::kRecordTimeoutMs - 1;
    }
    l.stream.push({req.back()});
    CHECK(l.receive() == PacketIOStatus::Ok);
}

TEST_CASE("PacketLink reset forgets the generation and anything half received")
{
    Link l;
    l.sync(1);
    const auto req = record(Kind::Packet, 1, {1, 2});
    l.stream.push(Bytes(req.begin(), req.begin() + 5));
    CHECK(l.receive() == PacketIOStatus::Incomplete);
    CHECK(l.link.reset() == PacketIOStatus::Ok);
    l.stream.push(record(Kind::Packet, 1, {3}));
    CHECK(l.receive() == PacketIOStatus::NoData);
    CHECK(l.stream.take() == error(1, Error::NotSynchronised));
}

TEST_CASE("FujiBusNative over a packet link reaches the core and answers")
{
    LinkedCore c;
    Bytes in = sync_record(9);
    const Bytes req = record(Kind::Packet, 9, request_for(kTestDevice, 0x42));
    in.insert(in.end(), req.begin(), req.end());
    const auto out = c.exchange(in);

    REQUIRE(out.size() == 2);
    CHECK(out[0].kind == Kind::SyncAck);
    CHECK(out[0].body == Bytes{PacketLink::kVersion, 0x00, 0x10}); // capacity 4096
    REQUIRE(out[1].kind == Kind::Packet);
    CHECK(out[1].generation == 9);
    const auto answer = fujinet::io::protocol::FujiBusPacket::fromRaw(out[1].body);
    REQUIRE(answer != nullptr);
    CHECK(static_cast<std::uint8_t>(answer->device()) == kTestDevice);
    CHECK(answer->command() == 0x42);
    REQUIRE(answer->paramCount() >= 1);
    CHECK(answer->param(0) == static_cast<std::uint32_t>(fujinet::io::StatusCode::Ok));
    REQUIRE(answer->data().has_value());
    CHECK(*answer->data() == Bytes(8, 0x5A));
}

TEST_CASE("A request the core can't parse is completed as Unanswered, and the link recovers")
{
    LinkedCore c;
    c.exchange(sync_record(1));
    Bytes bad = request_for(kTestDevice, 0x42);
    bad[4] ^= 0x5A; // a wrong FujiBus checksum; the link CRC is still valid
    const auto out = c.exchange(record(Kind::Packet, 1, bad));
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == Kind::Error);
    CHECK(out[0].body == Bytes{static_cast<std::uint8_t>(Error::Unanswered)});

    const auto next = c.exchange(record(Kind::Packet, 1, request_for(kTestDevice, 0x43)));
    REQUIRE(next.size() == 1);
    CHECK(next[0].kind == Kind::Packet);
}

TEST_CASE("An answer too large for the link is completed as Unanswered")
{
    // The request ran; its 200-byte answer exceeds a 64-byte link.
    LinkedCore c(64, 200);
    c.exchange(sync_record(1));
    const auto out = c.exchange(record(Kind::Packet, 1, request_for(kTestDevice, 0x42)));
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == Kind::Error);
    CHECK(out[0].body == Bytes{static_cast<std::uint8_t>(Error::Unanswered)});
}

TEST_CASE("with_packet_link wraps a stream only when asked")
{
    auto plain = fujinet::io::with_packet_link(false, std::make_unique<Stream>());
    REQUIRE(plain != nullptr);
    CHECK(plain->packet_io() == nullptr);

    auto linked = fujinet::io::with_packet_link(true, std::make_unique<Stream>());
    REQUIRE(linked != nullptr);
    REQUIRE(linked->packet_io() != nullptr);
    CHECK(linked->packet_io()->capacity() == PacketLink::kDefaultCapacity);

    CHECK(fujinet::io::with_packet_link(true, nullptr) == nullptr);
}
