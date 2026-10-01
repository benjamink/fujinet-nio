#include "doctest.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/diag/diagnostic_provider.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/packet_link.h"
#include "fujinet/io/devices/fuji_device.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using fujinet::config::FujiConfig;
using fujinet::config::FujiConfigStore;
using fujinet::diag::DiagArgsView;
using fujinet::diag::DiagStatus;
using fujinet::io::PacketLink;
using fujinet::io::PacketLinkChannel;

namespace {

class NullStream : public fujinet::io::Channel {
public:
    bool available() override { return false; }
    std::size_t read(std::uint8_t*, std::size_t) override { return 0; }
    void write(const std::uint8_t*, std::size_t) override {}
};

class MemoryStore final : public FujiConfigStore {
public:
    FujiConfig load() override { return saved; }
    void save(const FujiConfig& cfg) override
    {
        saved = cfg;
        ++saves;
    }
    FujiConfig saved;
    int saves{0};
};

fujinet::diag::DiagResult run(fujinet::diag::IDiagnosticProvider& p, std::vector<std::string_view> argv)
{
    DiagArgsView args;
    args.argv = std::move(argv);
    return p.execute(args);
}

} // namespace

TEST_CASE("link diagnostics exist only for a packet link channel")
{
    NullStream plain;
    CHECK(fujinet::diag::create_packet_link_diagnostic_provider(&plain, nullptr) == nullptr);
    CHECK(fujinet::diag::create_packet_link_diagnostic_provider(nullptr, nullptr) == nullptr);
}

TEST_CASE("link.status shows the link's state, settings and counters")
{
    PacketLinkChannel channel(std::make_unique<NullStream>(),
                              fujinet::io::PacketLinkSettings{.capacity = 2048, .recordTimeoutMs = 250});
    auto diag = fujinet::diag::create_packet_link_diagnostic_provider(&channel, nullptr);
    REQUIRE(diag != nullptr);
    CHECK(diag->provider_id() == "link");

    const auto r = run(*diag, {"link.status"});
    REQUIRE(r.status == DiagStatus::Ok);
    for (const char* expected : {"state: unsynchronised", "capacity: 2048", "record_timeout_ms: 250",
                                 "syncs: 0", "unanswered: 0", "abandoned: 0"}) {
        CAPTURE(expected);
        CHECK(r.text.find(expected) != std::string::npos);
    }
}

TEST_CASE("link.set changes the record timeout now and stores both settings")
{
    PacketLinkChannel channel(std::make_unique<NullStream>());
    auto storeOwned = std::make_unique<MemoryStore>();
    MemoryStore& store = *storeOwned;
    fujinet::io::FujiDevice fuji(nullptr, std::move(storeOwned));
    auto diag = fujinet::diag::create_packet_link_diagnostic_provider(&channel, &fuji);
    REQUIRE(diag != nullptr);

    CHECK(run(*diag, {"link.set", "record_timeout_ms", "750"}).status == DiagStatus::Ok);
    CHECK(channel.link().record_timeout_ms() == 750);
    CHECK(fuji.config().channel.packetLink.recordTimeoutMs == 750);

    // Capacity is fixed for the link's lifetime: stored for the next start.
    CHECK(run(*diag, {"link.set", "capacity", "1024"}).status == DiagStatus::Ok);
    CHECK(channel.link().capacity() == PacketLink::kDefaultCapacity);
    CHECK(fuji.config().channel.packetLink.capacity == 1024);

    CHECK(run(*diag, {"link.save"}).status == DiagStatus::Ok);
    CHECK(store.saves == 1);
    CHECK(store.saved.channel.packetLink.recordTimeoutMs == 750);
    CHECK(store.saved.channel.packetLink.capacity == 1024);
}

TEST_CASE("link.set rejects values outside the link's limits")
{
    PacketLinkChannel channel(std::make_unique<NullStream>());
    auto diag = fujinet::diag::create_packet_link_diagnostic_provider(&channel, nullptr);
    REQUIRE(diag != nullptr);
    CHECK(run(*diag, {"link.set", "record_timeout_ms", "0"}).status == DiagStatus::InvalidArgs);
    CHECK(run(*diag, {"link.set", "record_timeout_ms", "60001"}).status == DiagStatus::InvalidArgs);
    CHECK(run(*diag, {"link.set", "record_timeout_ms", "soon"}).status == DiagStatus::InvalidArgs);
    CHECK(run(*diag, {"link.set", "capacity", "63"}).status == DiagStatus::InvalidArgs);
    CHECK(run(*diag, {"link.set", "speed", "1"}).status == DiagStatus::InvalidArgs);
    CHECK(channel.link().record_timeout_ms() == PacketLink::kDefaultRecordTimeoutMs);

    // Without FujiDevice the timeout still changes live, but nothing can be stored.
    CHECK(run(*diag, {"link.set", "record_timeout_ms", "300"}).status == DiagStatus::Ok);
    CHECK(run(*diag, {"link.set", "capacity", "1024"}).status == DiagStatus::NotReady);
    CHECK(run(*diag, {"link.save"}).status == DiagStatus::NotReady);
}
