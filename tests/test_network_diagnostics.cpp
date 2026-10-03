// tests/test_network_diagnostics.cpp

#include "doctest.h"
#include "net_device_test_helpers.h"

#include "fujinet/core/core.h"
#include "fujinet/diag/diagnostic_provider.h"
#include "fujinet/io/devices/network_device_diagnostics.h"

#include <memory>
#include <string>
#include <string_view>

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
