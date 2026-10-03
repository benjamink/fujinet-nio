// tests/test_network_diagnostics.cpp

#include "doctest.h"
#include "net_device_test_helpers.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/core/core.h"
#include "fujinet/core/device_init.h"
#include "fujinet/diag/diagnostic_provider.h"
#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/io/devices/network_device_diagnostics.h"
#include "fujinet/platform/network_registry.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace fujinet::tests::netdev;
using fujinet::io::ContentTranslationType;
using fujinet::io::NetworkDeviceDiagnosticsAccessor;

namespace {

struct Fixture {
    fujinet::core::FujinetCore core;
    NetworkDevice* dev{nullptr};
    std::uint16_t deviceId{to_device_id(WireDeviceId::NetworkService)};

    Fixture()
    {
        auto d = std::make_unique<NetworkDevice>(make_stub_registry_http_only());
        dev = d.get();
        REQUIRE(core.deviceManager().registerDevice(deviceId, std::move(d)));
    }

    std::string sessions()
    {
        auto provider = fujinet::diag::create_network_diagnostic_provider(core);
        fujinet::diag::DiagArgsView args;
        args.argv = {std::string_view("net.sessions")};
        auto res = provider->execute(args);
        REQUIRE(res.status == fujinet::diag::DiagStatus::Ok);
        return res.text;
    }
};

} // namespace

TEST_CASE("net.sessions shows translation none for a plain session")
{
    Fixture f;
    open_handle_stub(*f.dev, f.deviceId, "http://example.com/plain");

    const std::string text = f.sessions();
    CHECK(text.find("url=http://example.com/plain translation=none\r\n") != std::string::npos);
    CHECK(text.find("selector=") == std::string::npos);
}

TEST_CASE("net.sessions shows JSON translation state, ready after the body is translated")
{
    Fixture f;
    const auto handle = open_handle_stub(
        *f.dev, f.deviceId, "http://example.com/json", 1, 0, 0, {},
        ContentTranslationType::Json, "/url");

    CHECK(f.sessions().find(" translation=json selector=/url ready=0 translated=0\r\n") != std::string::npos);

    REQUIRE(info_req(*f.dev, f.deviceId, handle).status == StatusCode::Ok);

    const std::string size = std::to_string(std::string("http://example.com/json").size());
    CHECK(f.sessions().find(" translation=json selector=/url ready=1 translated=" + size + "\r\n") !=
          std::string::npos);
}

TEST_CASE("net.sessions shows an Image session with its selector")
{
    Fixture f;
    open_handle_stub(
        *f.dev, f.deviceId, "http://example.com/a.png", 1, 0, 0, {},
        ContentTranslationType::Image, "w=640,h=400,colors=16");

    const std::string text = f.sessions();
    CHECK(text.find(" translation=image selector=w=640,h=400,colors=16 ready=0 translated=0\r\n") !=
          std::string::npos);
}

TEST_CASE("SessionRow carries the translation fields from the session")
{
    Fixture f;
    open_handle_stub(
        *f.dev, f.deviceId, "http://example.com/a.png", 1, 0, 0, {},
        ContentTranslationType::Image, "w=320");

    const auto rows = NetworkDeviceDiagnosticsAccessor::sessions(*f.dev);
    bool found = false;
    for (const auto& r : rows) {
        if (!r.active) continue;
        found = true;
        CHECK(r.translationType == 4);
        CHECK(r.translationSelector == "w=320");
        CHECK_FALSE(r.translationReady);
        CHECK(r.translatedSize == 0);
    }
    CHECK(found);
}

// ---------------------------------------------------------------------------
// net.image.*: the Image translator's pixel cap (network.image_max_pixels)
// ---------------------------------------------------------------------------

namespace {

class MemoryStore final : public fujinet::config::FujiConfigStore {
public:
    fujinet::config::FujiConfig load() override { return saved; }
    void save(const fujinet::config::FujiConfig& cfg) override
    {
        saved = cfg;
        ++saves;
    }
    fujinet::config::FujiConfig saved;
    int saves{0};
};

fujinet::diag::DiagResult run(fujinet::diag::IDiagnosticProvider& p, std::vector<std::string_view> argv)
{
    fujinet::diag::DiagArgsView args;
    args.argv = std::move(argv);
    return p.execute(args);
}

} // namespace

TEST_CASE("POSIX supplies a 4096x4096 default image pixel cap")
{
    CHECK(fujinet::platform::default_image_max_pixels() == 4096u * 4096u);
}

TEST_CASE("network.image_max_pixels 0 means the platform default when the device is registered")
{
    fujinet::config::NetworkConfig config;
    CHECK(fujinet::core::network_device_settings(config).imageMaxPixels ==
          fujinet::platform::default_image_max_pixels());
    config.imageMaxPixels = 300000;
    CHECK(fujinet::core::network_device_settings(config).imageMaxPixels == 300000u);

    fujinet::core::FujinetCore core;
    fujinet::core::register_network_device(core, config);
    auto* dev = dynamic_cast<NetworkDevice*>(
        core.deviceManager().getDevice(to_device_id(WireDeviceId::NetworkService)));
    REQUIRE(dev != nullptr);
    CHECK(NetworkDeviceDiagnosticsAccessor::image_max_pixels(*dev) == 300000u);
}

TEST_CASE("net.image.get shows the live cap, the stored value and the platform default")
{
    Fixture f;
    NetworkDeviceDiagnosticsAccessor::set_image_max_pixels(*f.dev, 490000);
    auto storeOwned = std::make_unique<MemoryStore>();
    fujinet::io::FujiDevice fuji(nullptr, std::move(storeOwned));
    auto ctx = std::make_shared<fujinet::diag::NetworkDiagWifiContext>();
    ctx->fuji = &fuji;
    auto provider = fujinet::diag::create_network_diagnostic_provider(f.core, ctx);

    const auto r = run(*provider, {"net.image.get"});
    REQUIRE(r.status == fujinet::diag::DiagStatus::Ok);
    CHECK(r.text == "image_max_pixels: 490000\r\n"
                    "stored_image_max_pixels: default\r\n"
                    "platform_default_image_max_pixels: 16777216\r\n");
}

TEST_CASE("net.image.set applies to new sessions now and stores the value; net.image.save writes it")
{
    Fixture f;
    auto storeOwned = std::make_unique<MemoryStore>();
    MemoryStore& store = *storeOwned;
    fujinet::io::FujiDevice fuji(nullptr, std::move(storeOwned));
    auto ctx = std::make_shared<fujinet::diag::NetworkDiagWifiContext>();
    ctx->fuji = &fuji;
    auto provider = fujinet::diag::create_network_diagnostic_provider(f.core, ctx);

    CHECK(run(*provider, {"net.image.set", "max_pixels", "250000"}).status == fujinet::diag::DiagStatus::Ok);
    CHECK(NetworkDeviceDiagnosticsAccessor::image_max_pixels(*f.dev) == 250000u);
    CHECK(fuji.config().network.imageMaxPixels == 250000u);
    CHECK(run(*provider, {"net.image.get"}).text.find("stored_image_max_pixels: 250000\r\n") != std::string::npos);

    CHECK(run(*provider, {"net.image.save"}).status == fujinet::diag::DiagStatus::Ok);
    CHECK(store.saves == 1);
    CHECK(store.saved.network.imageMaxPixels == 250000u);

    // "default" stores 0 and goes back to the platform's value.
    CHECK(run(*provider, {"net.image.set", "max_pixels", "default"}).status == fujinet::diag::DiagStatus::Ok);
    CHECK(NetworkDeviceDiagnosticsAccessor::image_max_pixels(*f.dev) == fujinet::platform::default_image_max_pixels());
    CHECK(fuji.config().network.imageMaxPixels == 0u);
}

TEST_CASE("net.image.set rejects bad values; without FujiDevice it is live only and save is not ready")
{
    Fixture f;
    auto provider = fujinet::diag::create_network_diagnostic_provider(f.core);
    const std::uint32_t before = NetworkDeviceDiagnosticsAccessor::image_max_pixels(*f.dev);

    CHECK(run(*provider, {"net.image.set", "max_pixels", "0"}).status == fujinet::diag::DiagStatus::InvalidArgs);
    CHECK(run(*provider, {"net.image.set", "max_pixels", "67108865"}).status == fujinet::diag::DiagStatus::InvalidArgs);
    CHECK(run(*provider, {"net.image.set", "max_pixels", "lots"}).status == fujinet::diag::DiagStatus::InvalidArgs);
    CHECK(run(*provider, {"net.image.set", "speed", "1"}).status == fujinet::diag::DiagStatus::InvalidArgs);
    CHECK(run(*provider, {"net.image.set"}).status == fujinet::diag::DiagStatus::InvalidArgs);
    CHECK(NetworkDeviceDiagnosticsAccessor::image_max_pixels(*f.dev) == before);

    CHECK(run(*provider, {"net.image.set", "max_pixels", "1000"}).status == fujinet::diag::DiagStatus::Ok);
    CHECK(NetworkDeviceDiagnosticsAccessor::image_max_pixels(*f.dev) == 1000u);
    const auto get = run(*provider, {"net.image.get"});
    CHECK(get.text.find("stored_image_max_pixels") == std::string::npos);
    CHECK(run(*provider, {"net.image.save"}).status == fujinet::diag::DiagStatus::NotReady);
}
