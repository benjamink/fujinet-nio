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

#include <chrono>
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
    // When set, wait_for_readable() with nothing to read blocks for the whole
    // timeout on this clock.
    std::uint64_t* clock{nullptr};

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
    bool supports_readable_wait() const override { return clock != nullptr; }
    bool wait_for_readable(std::chrono::milliseconds timeout) override
    {
        if (_rx.empty() && clock) *clock += static_cast<std::uint64_t>(timeout.count());
        return !_rx.empty();
    }

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

// SyncAck from a peer of capacity 64 and the default record timeout.
Bytes sync_ack(std::uint8_t gen, std::uint16_t timeout_ms = PacketLink::kDefaultRecordTimeoutMs)
{
    return record(Kind::SyncAck, gen,
                  {PacketLink::kVersion, 64, 0, static_cast<std::uint8_t>(timeout_ms),
                   static_cast<std::uint8_t>(timeout_ms >> 8)});
}

struct Link {
    Link() { stream.clock = &now; }
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
    CHECK(record(Kind::SyncAck, 7, {0x01, 0x00, 0x10, 0x64, 0x00}) ==
          Bytes{0xF5, 0x4E, 0x11, 0x07, 0x05, 0x00, 0x01, 0x00, 0x10, 0x64, 0x00, 0x41, 0x40, 0x98, 0x25});
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

TEST_CASE("PacketLink Sync and SyncAck carry the link version, capacity and record timeout")
{
    Link l;
    l.stream.push(sync_record(3, 1));
    CHECK(l.receive() == PacketIOStatus::NoData);
    CHECK(l.stream.take() == sync_ack(3));

    // A controller that speaks a later version is answered with this one.
    l.stream.push(sync_record(4, 9));
    CHECK(l.receive() == PacketIOStatus::NoData);
    CHECK(l.stream.take() == record(Kind::SyncAck, 4, {1, 64, 0, 100, 0}));
}

TEST_CASE("PacketLink advertises a changed record timeout at the next Sync")
{
    Link l;
    l.sync(1);
    l.link.set_record_timeout_ms(2500);
    CHECK(l.link.record_timeout_ms() == 2500);
    l.stream.push(sync_record(2));
    CHECK(l.receive() == PacketIOStatus::NoData);
    CHECK(l.stream.take() == sync_ack(2, 2500));

    // Kept within 1..kMaxRecordTimeoutMs.
    l.link.set_record_timeout_ms(0);
    CHECK(l.link.record_timeout_ms() == 1);
    l.link.set_record_timeout_ms(1000000);
    CHECK(l.link.record_timeout_ms() == PacketLink::kMaxRecordTimeoutMs);
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
    l.now += PacketLink::kDefaultRecordTimeoutMs;
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
    // A record cut off part way would take the next record's bytes as its
    // own: here its header claims 40 bytes, more than a Sync supplies. The
    // controller's quiet pause ends it, whether NIO polled through the pause
    // or waited on the stream.
    for (const bool polled : {true, false}) {
        CAPTURE(polled);
        Link l;
        const auto cut = record(Kind::Packet, 1, Bytes(40, 0x11));
        l.stream.push(Bytes(cut.begin(), cut.begin() + 8));
        CHECK(l.receive() == PacketIOStatus::Incomplete);
        if (polled) {
            l.now += PacketLink::kDefaultRecordTimeoutMs;
            CHECK(l.receive() == PacketIOStatus::NoData);
        } else {
            CHECK_FALSE(l.link.wait_for_readable(std::chrono::milliseconds(PacketLink::kDefaultRecordTimeoutMs)));
        }
        l.stream.push(sync_record(5));
        CHECK(l.receive() == PacketIOStatus::NoData);
        CHECK(l.stream.take() == sync_ack(5));
        CHECK(l.link.stats().abandoned == 1);
    }
}

TEST_CASE("PacketLink: time NIO spends busy is not a gap in the record")
{
    // More of the record arrived while NIO was busy elsewhere and waited in
    // the stream's buffer. Reading it late is not a gap on the wire, so the
    // record survives until its last bytes arrive.
    Link l;
    l.sync(1);
    const auto req = record(Kind::Packet, 1, {1, 2, 3});
    l.stream.push(Bytes(req.begin(), req.begin() + 5));
    CHECK(l.receive() == PacketIOStatus::Incomplete);
    l.stream.push(Bytes(req.begin() + 5, req.end() - 2));
    l.now += 10 * PacketLink::kDefaultRecordTimeoutMs; // busy, not waiting
    CHECK(l.receive() == PacketIOStatus::Incomplete);
    l.stream.push(Bytes(req.end() - 2, req.end()));
    CHECK(l.receive() == PacketIOStatus::Ok);
    CHECK(l.link.stats().abandoned == 0);
}

TEST_CASE("PacketLink: a short wait doesn't end a record")
{
    Link l;
    l.sync(1);
    const auto req = record(Kind::Packet, 1, {1, 2, 3});
    l.stream.push(Bytes(req.begin(), req.begin() + 5));
    CHECK(l.receive() == PacketIOStatus::Incomplete);
    l.link.wait_for_readable(std::chrono::milliseconds(PacketLink::kDefaultRecordTimeoutMs - 1));
    l.stream.push(Bytes(req.begin() + 5, req.end()));
    CHECK(l.receive() == PacketIOStatus::Ok);
}

TEST_CASE("PacketLink counts what it sees")
{
    Link l;
    l.sync(1);
    auto bad = record(Kind::Packet, 1, {1});
    bad[7] ^= 0xFF;
    l.stream.push(bad);
    l.stream.push(record(Kind::Packet, 9, {1})); // wrong generation
    l.stream.push(record(Kind::Packet, 1, {2}));
    CHECK(l.receive() == PacketIOStatus::Ok);
    CHECK(l.receive() == PacketIOStatus::NoData); // unanswered
    const Bytes answer{0xAA};
    l.stream.push(record(Kind::Packet, 1, {3}));
    CHECK(l.receive() == PacketIOStatus::Ok);
    CHECK(l.link.send(answer.data(), answer.size()) == PacketIOStatus::Ok);

    const auto& st = l.link.stats();
    CHECK(st.syncs == 1);
    CHECK(st.corrupt == 1);
    CHECK(st.refused == 1);
    CHECK(st.requests == 2);
    CHECK(st.unanswered == 1);
    CHECK(st.answers == 1);
    CHECK(l.link.state_name() == "idle");
    CHECK(l.link.generation() == 1);
    CHECK(l.link.version() == PacketLink::kVersion);
}

TEST_CASE("packet_link_settings: zero means the default, networks wait longer, values are clamped")
{
    using fujinet::io::packet_link_settings;
    const auto uart = packet_link_settings(0, 0, false);
    CHECK(uart.capacity == PacketLink::kDefaultCapacity);
    CHECK(uart.recordTimeoutMs == PacketLink::kDefaultRecordTimeoutMs);
    const auto tcp = packet_link_settings(0, 0, true);
    CHECK(tcp.recordTimeoutMs == PacketLink::kDefaultNetworkRecordTimeoutMs);
    const auto set = packet_link_settings(1024, 250, true);
    CHECK(set.capacity == 1024);
    CHECK(set.recordTimeoutMs == 250);
    const auto clamped = packet_link_settings(10, 999999, false);
    CHECK(clamped.capacity == PacketLink::kMinCapacity);
    CHECK(clamped.recordTimeoutMs == PacketLink::kMaxRecordTimeoutMs);
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
        l.now += PacketLink::kDefaultRecordTimeoutMs - 1;
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
    CHECK(out[0].body == Bytes{PacketLink::kVersion, 0x00, 0x10, 100, 0}); // capacity 4096, 100 ms
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
