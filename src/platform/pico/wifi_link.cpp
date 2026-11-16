#include "fujinet/platform/pico/wifi_link.h"

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "pico/cyw43_arch.h"
#include "fujinet/core/logging.h"

// CYW43 power-save mode, chosen at build time by pico2w/CMakeLists.txt (NIO_PICO_WIFI_PM).
// The default is the driver's own, so an unset build behaves as before.
#ifndef NIO_PICO_WIFI_PM
#define NIO_PICO_WIFI_PM CYW43_DEFAULT_PM
#endif

namespace fujinet::platform::pico {

namespace {

std::string ip_text(const ip4_addr_t& a) { return ip4_addr_isany_val(a) ? std::string{} : std::string(ip4addr_ntoa(&a)); }

} // namespace

net::LinkState PicoWifiLink::state() const { return _state; }

void PicoWifiLink::set_bssid(std::string bssid) { _bssid = std::move(bssid); }

void PicoWifiLink::connect(std::string ssid, std::string pass)
{
    _ssid = std::move(ssid);
    _password = std::move(pass);
    cyw43_arch_enable_sta_mode();
    // enable_sta_mode applies CYW43_DEFAULT_PM; replace it with the configured mode.
    cyw43_wifi_pm(&cyw43_state, NIO_PICO_WIFI_PM);
    FN_LOGI("wifi", "power-save mode 0x%08lx", static_cast<unsigned long>(NIO_PICO_WIFI_PM));
    // Non-blocking: poll() watches the join. BSSID pinning is not exposed by cyw43_arch yet.
    const uint32_t auth = _password.empty() ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_MIXED_PSK;
    const int rc = cyw43_arch_wifi_connect_async(_ssid.c_str(), _password.empty() ? nullptr : _password.c_str(), auth);
    _join_started = (rc == 0);
    _state = _join_started ? net::LinkState::Connecting : net::LinkState::Failed;
}

void PicoWifiLink::disconnect()
{
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    _join_started = false;
    _state = net::LinkState::Disconnected;
}

void PicoWifiLink::poll()
{
    if (!_join_started) return;
    const int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (st == CYW43_LINK_UP) {
        _state = net::LinkState::Connected;
    } else if (st < 0) { // CYW43_LINK_FAIL / NONET / BADAUTH
        _state = net::LinkState::Failed;
        _join_started = false;
    } else {
        _state = net::LinkState::Connecting;
    }
}

std::string PicoWifiLink::ip_address() const
{
    return netif_default ? ip_text(*netif_ip4_addr(netif_default)) : std::string{};
}

std::string PicoWifiLink::subnet_mask() const
{
    return netif_default ? ip_text(*netif_ip4_netmask(netif_default)) : std::string{};
}

std::string PicoWifiLink::gateway() const
{
    return netif_default ? ip_text(*netif_ip4_gw(netif_default)) : std::string{};
}

std::string PicoWifiLink::dns_server() const
{
    return {}; // dns_getserver() once the lwIP socket ops land
}

bool PicoWifiLink::active_credentials(net::WifiCredentials& out) const
{
    if (_state != net::LinkState::Connected) return false;
    out.ssid = _ssid;
    out.password = _password;
    out.bssid = _bssid;
    return true;
}

net::WifiBssid PicoWifiLink::current_bssid() const { return {}; }

net::MacAddress PicoWifiLink::mac_address() const
{
    net::MacAddress mac{};
    if (cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac.bytes) == 0) mac.valid = true;
    return mac;
}

std::int8_t PicoWifiLink::rssi() const
{
    int32_t v = 0;
    if (cyw43_wifi_get_rssi(&cyw43_state, &v) != 0) return 0;
    return static_cast<std::int8_t>(v);
}

net::WifiLinkCapabilities PicoWifiLink::capabilities() const
{
    return {net::WifiCapabilityConfig | net::WifiCapabilityStatus | net::WifiCapabilityConnect |
                net::WifiCapabilityDisconnect,
            net::WifiBackendKind::Unavailable /* a Pico backend id comes with the wire-format change */};
}

bool PicoWifiLink::supports_scan() const { return false; }

net::WifiScanResult PicoWifiLink::scan_wifi() { return {}; }

} // namespace fujinet::platform::pico
