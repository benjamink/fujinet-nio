#include "doctest.h"

#include "fujinet/io/core/channel.h"
#include "fujinet/tnfs/tnfs_protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace fujinet::tnfs;

namespace {

std::vector<std::uint8_t> pattern(std::size_t n)
{
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xFF);
    return v;
}

} // namespace

namespace {

// A TNFS server behind a datagram Channel whose reply queue is small and drops what does
// not fit, like a Pico lwIP UDP socket. It serves one file.
class FakeFileChannel final : public fujinet::io::Channel {
public:
    FakeFileChannel(std::vector<std::uint8_t> content, std::size_t queueDepth)
        : _content(std::move(content)), _depth(queueDepth) {}

    std::size_t dropped = 0;
    std::size_t datagramsSent = 0;

    bool available() override { return !_replies.empty(); }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override
    {
        if (_replies.empty()) return 0;
        auto& r = _replies.front();
        const std::size_t n = std::min(maxLen, r.size());
        std::memcpy(buffer, r.data(), n);
        _replies.pop_front();
        return n;
    }

    // What the channel says it can queue; the client must keep within it.
    std::size_t receive_datagram_capacity() const override { return _depth; }

    void write(const std::uint8_t* b, std::size_t len) override
    {
        if (len < 4) return;
        ++datagramsSent;
        const std::uint8_t seq = b[2];
        const std::uint8_t cmd = b[3];
        const std::uint8_t* p = b + 4;
        std::vector<std::uint8_t> pl;
        switch (cmd) {
        case CMD_MOUNT:
            pl = {0, 2, 1, 0xE8, 0x03};
            break;
        case CMD_OPEN: {
            const int fd = _next++;
            _pos[fd] = 0;
            pl = {0, static_cast<std::uint8_t>(fd)};
            break;
        }
        case CMD_LSEEK:
            _pos[p[0]] = p[2] | (p[3] << 8) | (p[4] << 16) | (static_cast<std::uint32_t>(p[5]) << 24);
            pl = {0};
            break;
        case CMD_READ: {
            const std::uint32_t at = _pos[p[0]];
            const std::size_t want = p[1] | (p[2] << 8);
            if (at >= _content.size()) { pl = {RESULT_END_OF_FILE}; break; }
            const std::size_t n = std::min(want, _content.size() - at);
            pl = {0, static_cast<std::uint8_t>(n & 0xFF), static_cast<std::uint8_t>(n >> 8)};
            pl.insert(pl.end(), _content.begin() + at, _content.begin() + at + n);
            _pos[p[0]] = at + static_cast<std::uint32_t>(n);
            break;
        }
        default:
            pl = {0}; // CLOSE and anything else
            break;
        }
        std::vector<std::uint8_t> reply = {static_cast<std::uint8_t>(kSession & 0xFF),
                                           static_cast<std::uint8_t>(kSession >> 8), seq, cmd};
        reply.insert(reply.end(), pl.begin(), pl.end());
        if (_replies.size() >= _depth) { ++dropped; return; } // the socket's queue is full
        _replies.push_back(std::move(reply));
    }

private:
    static constexpr std::uint16_t kSession = 0x1234;
    std::vector<std::uint8_t> _content;
    std::size_t _depth;
    std::deque<std::vector<std::uint8_t>> _replies;
    std::map<int, std::uint32_t> _pos;
    int _next = 3;
};

} // namespace

TEST_CASE("TNFS read-ahead keeps no more requests in flight than the channel can queue")
{
    constexpr std::size_t kBlocks = 8;
    const auto data = pattern(kBlocks * 512);

    for (std::size_t depth : {std::size_t{1}, std::size_t{4}, std::size_t{8}, std::size_t{16}}) {
        CAPTURE(depth);
        auto channel = std::make_unique<FakeFileChannel>(data, depth);
        FakeFileChannel* raw = channel.get();
        auto client = make_udp_tnfs_client(std::move(channel));
        REQUIRE(client->supports_parallel_reads());
        REQUIRE(client->mount("/", "", ""));

        std::vector<int> handles;
        for (std::size_t i = 0; i < kBlocks; ++i) {
            const int h = client->open("/f", 1, 0);
            REQUIRE(h >= 0);
            handles.push_back(h);
        }

        std::vector<std::uint8_t> out(kBlocks * 512);
        std::vector<ITnfsClient::ReadAt> reads(kBlocks);
        for (std::size_t i = 0; i < kBlocks; ++i) {
            reads[i].fileHandle = handles[i];
            reads[i].offset = static_cast<std::uint32_t>(i * 512);
            reads[i].dst = out.data() + i * 512;
            reads[i].bytes = 512;
        }
        client->read_parallel(reads);

        // Nothing lost means no timeout was waited out, and every block arrived intact.
        CHECK(raw->dropped == 0);
        for (std::size_t i = 0; i < kBlocks; ++i) {
            CHECK(reads[i].got == 512);
        }
        CHECK(out == data);
    }
}

TEST_CASE("TNFS read-ahead with an unbounded channel sends everything at once")
{
    // The default capacity is unbounded, so platforms with kernel socket buffers are unchanged:
    // one batch, one round of lseeks and one of reads.
    const auto data = pattern(8 * 512);
    auto channel = std::make_unique<FakeFileChannel>(data, 1000);
    FakeFileChannel* raw = channel.get();
    auto client = make_udp_tnfs_client(std::move(channel));
    REQUIRE(client->mount("/", "", ""));
    std::vector<int> handles;
    for (int i = 0; i < 8; ++i) handles.push_back(client->open("/f", 1, 0));
    const std::size_t before = raw->datagramsSent;

    std::vector<std::uint8_t> out(8 * 512);
    std::vector<ITnfsClient::ReadAt> reads(8);
    for (std::size_t i = 0; i < 8; ++i) {
        reads[i] = {};
        reads[i].fileHandle = handles[i];
        reads[i].offset = static_cast<std::uint32_t>(i * 512);
        reads[i].dst = out.data() + i * 512;
        reads[i].bytes = 512;
    }
    client->read_parallel(reads);
    CHECK(raw->datagramsSent - before == 16); // 8 lseeks + 8 reads, none repeated
    CHECK(out == data);
}
