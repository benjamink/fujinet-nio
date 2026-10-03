#include "doctest.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/devices/fuji_commands.h"
#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/build/profile.h"
#include "fujinet/core/version.h"

#include <string>
#include <string_view>
#include <vector>

#include <memory>

namespace {

using fujinet::config::FujiConfig;
using fujinet::config::FujiConfigStore;
using fujinet::io::FujiDevice;
using fujinet::io::IORequest;
using fujinet::io::StatusCode;
using fujinet::io::protocol::FujiCommand;

class MemoryFujiConfigStore final : public FujiConfigStore {
public:
    explicit MemoryFujiConfigStore(FujiConfig initial)
        : config(std::move(initial))
    {
    }

    FujiConfig load() override
    {
        ++loadCount;
        return config;
    }

    void save(const FujiConfig& cfg) override
    {
        config = cfg;
        ++saveCount;
    }

    FujiConfig config;
    int loadCount{0};
    int saveCount{0};
};

} // namespace

TEST_CASE("FujiDevice loads non-mount configuration on start")
{
    FujiConfig initial;
    initial.general.deviceName = "test-fujinet";
    auto store = std::make_unique<MemoryFujiConfigStore>(initial);
    auto* storePtr = store.get();
    FujiDevice device(nullptr, std::move(store));

    device.start();

    CHECK(storePtr->loadCount == 1);
    CHECK(device.config().general.deviceName == "test-fujinet");
}

TEST_CASE("FujiDevice GetInfo reports the firmware version and build profile")
{
    FujiDevice device(nullptr, nullptr);
    IORequest request;
    request.command = static_cast<std::uint16_t>(FujiCommand::GetInfo);
    request.payload = {1};
    const auto response = device.handle(request);
    REQUIRE(response.status == StatusCode::Ok);

    const std::string firmware = fujinet::version();
    const std::string_view profile = fujinet::build::current_build_profile().name;
    std::vector<std::uint8_t> expected{1, static_cast<std::uint8_t>(firmware.size())};
    expected.insert(expected.end(), firmware.begin(), firmware.end());
    expected.push_back(static_cast<std::uint8_t>(profile.size()));
    expected.insert(expected.end(), profile.begin(), profile.end());
    CHECK(response.payload == expected);
}

TEST_CASE("FujiDevice GetInfo answers newer clients in its own version")
{
    FujiDevice device(nullptr, nullptr);
    IORequest request;
    request.command = static_cast<std::uint16_t>(FujiCommand::GetInfo);
    request.payload = {1};
    const auto v1 = device.handle(request).payload;

    // A client that understands a later version, or sends fields this
    // firmware doesn't know, gets the version-1 reply.
    for (const std::vector<std::uint8_t>& payload : {std::vector<std::uint8_t>{2},
                                                     std::vector<std::uint8_t>{1, 0xAA, 0xBB},
                                                     std::vector<std::uint8_t>{9, 0xAA}}) {
        request.payload = payload;
        const auto response = device.handle(request);
        CHECK(response.status == StatusCode::Ok);
        CHECK(response.payload == v1);
    }
}

TEST_CASE("FujiDevice GetInfo needs a version")
{
    FujiDevice device(nullptr, nullptr);
    IORequest request;
    request.command = static_cast<std::uint16_t>(FujiCommand::GetInfo);
    for (const std::vector<std::uint8_t>& payload : {std::vector<std::uint8_t>{},
                                                     std::vector<std::uint8_t>{0}}) {
        request.payload = payload;
        CHECK(device.handle(request).status == StatusCode::InvalidRequest);
    }
}

TEST_CASE("FujiDevice reset invokes the platform reset handler")
{
    bool resetCalled = false;
    auto store = std::make_unique<MemoryFujiConfigStore>(FujiConfig{});
    FujiDevice device([&resetCalled] { resetCalled = true; }, std::move(store));

    IORequest request{};
    request.command = static_cast<std::uint16_t>(FujiCommand::Reset);
    const auto response = device.handle(request);

    CHECK(response.status == StatusCode::Ok);
    CHECK(resetCalled);
}

TEST_CASE("FujiDevice rejects retired mount configuration commands")
{
    auto store = std::make_unique<MemoryFujiConfigStore>(FujiConfig{});
    FujiDevice device(nullptr, std::move(store));

    for (const std::uint16_t command : {0xFDU, 0xFCU, 0xFBU}) {
        IORequest request{};
        request.command = command;
        CHECK(device.handle(request).status == StatusCode::Unsupported);
    }
}
