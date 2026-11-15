#pragma once

#include <string>

#include "fujinet/net/network_link.h"

namespace fujinet::platform::pico {

// The Pico 2 W's CYW43439 radio, through the Pico SDK's cyw43_arch (lwIP).
// cyw43_arch_init() must have run (main_pico.cpp does it).
class PicoWifiLink final : public net::INetworkLink {
public:
    net::LinkState state() const override;
    void connect(std::string ssid, std::string pass) override;
    void set_bssid(std::string bssid) override;
    void disconnect() override;
    void poll() override;
    std::string ip_address() const override;
    net::WifiBssid current_bssid() const override;
    bool active_credentials(net::WifiCredentials& out) const override;
    net::MacAddress mac_address() const override;
    std::int8_t rssi() const override;
    std::string subnet_mask() const override;
    std::string gateway() const override;
    std::string dns_server() const override;
    net::WifiLinkCapabilities capabilities() const override;
    bool supports_scan() const override;
    net::WifiScanResult scan_wifi() override;

private:
    std::string _ssid;
    std::string _password;
    std::string _bssid;
    net::LinkState _state{net::LinkState::Disconnected};
    bool _join_started{false};
};

} // namespace fujinet::platform::pico
