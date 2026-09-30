#include "doctest.h"

#include "fujibus_wire_fixtures.h"

#include "fujinet/build/profile.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/packet_link.h"

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

struct Link {
    Stream stream;
    std::uint64_t now = 0;
    PacketLink link{stream, 64, [this] { return now; }};
    std::uint8_t buf[64];

    PacketIOStatus receive() { return link.receive(buf, sizeof(buf)).status; }

    void sync(std::uint8_t gen)
    {
        stream.push(record(Kind::Sync, gen));
        CHECK(receive() == PacketIOStatus::NoData);
        CHECK(stream.take() == record(Kind::SyncAck, gen));
    }
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
    CHECK(record(Kind::Sync, 7) == Bytes{0xF5, 0x4E, 0x10, 0x07, 0x00, 0x00, 0x06, 0x9E, 0x12, 0x74});
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

TEST_CASE("PacketLink allows one outstanding request")
{
    Link l;
    l.sync(1);
    l.stream.push(record(Kind::Packet, 1, {1}));
    CHECK(l.receive() == PacketIOStatus::Ok);
    l.stream.push(record(Kind::Packet, 1, {2}));
    CHECK(l.receive() == PacketIOStatus::NoData);
    CHECK(l.stream.take() == error(1, Error::Busy));
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
    const auto sync = record(Kind::Sync, 4);
    l.stream.push(Bytes(sync.begin() + 1, sync.end()));
    CHECK(l.receive() == PacketIOStatus::NoData);
    CHECK(l.stream.take() == record(Kind::SyncAck, 4));

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

TEST_CASE("PacketLink drops an answer to a request from an earlier generation")
{
    Link l;
    l.sync(1);
    l.stream.push(record(Kind::Packet, 1, {1}));
    CHECK(l.receive() == PacketIOStatus::Ok);
    l.sync(2);
    const Bytes response{0xAA};
    CHECK(l.link.send(response.data(), response.size()) == PacketIOStatus::SendFailed);
    CHECK(l.stream.take().empty());
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
    fujinet::build::BuildProfile profile{};
    profile.primaryTransport = fujinet::build::TransportKind::FujiBusNative;
    auto owned = std::make_unique<Stream>();
    Stream& stream = *owned;
    PacketLinkChannel channel(std::move(owned));

    fujinet::core::FujinetCore core;
    REQUIRE(fujinet::core::setup_transports(core, channel, profile) != nullptr);

    stream.push(record(Kind::Sync, 9));
    stream.push(record(Kind::Packet, 9, fujibus_wire_fixtures::minimum.raw));
    core.tick();
    core.tick();

    const auto out = stream.take();
    const auto ack = record(Kind::SyncAck, 9);
    REQUIRE(out.size() > ack.size() + 6);
    CHECK(Bytes(out.begin(), out.begin() + ack.size()) == ack);
    const Bytes answer(out.begin() + ack.size(), out.end());
    CHECK(answer[2] == static_cast<std::uint8_t>(Kind::Packet));
    CHECK(answer[3] == 9);
    CHECK(answer[6] == fujibus_wire_fixtures::minimum.raw[0]); // the device answers
}
