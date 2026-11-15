#include "fujinet/platform/network_registry.h"

#include "fujinet/io/devices/network_protocol_stub.h"

namespace fujinet::platform {

// tcp/tls/http/https are stubs until the lwIP and mbedTLS backends exist.
io::ProtocolRegistry make_default_network_registry()
{
    io::ProtocolRegistry r;
    for (const char* scheme : {"tcp", "tls", "http", "https"}) {
        r.register_scheme(scheme, [] { return std::make_unique<io::StubNetworkProtocol>(); });
    }
    return r;
}

} // namespace fujinet::platform
