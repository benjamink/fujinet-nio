#include "fujinet/platform/network_registry.h"
#include "fujinet/io/devices/network_protocol_stub.h"

#include "fujinet/platform/esp32/tcp_network_protocol_espidf.h"
#include "fujinet/platform/esp32/tls_network_protocol_espidf.h"
#include "fujinet/platform/esp32/http_network_protocol_espidf.h"

namespace fujinet::platform {

io::ProtocolRegistry make_default_network_registry()
{
    io::ProtocolRegistry r;

    // TCP stream sockets (ESP-IDF / lwIP)
    r.register_scheme("tcp", [] {
        return std::make_unique<esp32::TcpNetworkProtocolEspIdf>();
    });

    // TLS over TCP (secure sockets using esp_tls)
    r.register_scheme("tls", [] {
        return std::make_unique<esp32::TlsNetworkProtocolEspIdf>();
    });

    r.register_scheme("http", [] { return std::make_unique<esp32::HttpNetworkProtocolEspIdf>(); });
    r.register_scheme("https", [] { return std::make_unique<esp32::HttpNetworkProtocolEspIdf>(); });

    return r;
}

std::uint32_t default_image_max_pixels()
{
    // stb_image's PNG path holds the compressed IDAT copy, the inflate buffer
    // and the decoded RGB at once, roughly 3-4 bytes per pixel at peak, on top
    // of the cached response body. 700x700 (0.49 Mpx) keeps that near 2 MB of
    // PSRAM. See docs/network_device_protocol.md for the measured time.
    return 700u * 700u;
}

} // namespace fujinet::platform
