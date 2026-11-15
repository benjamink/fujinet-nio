#include "fujinet/platform/network_registry.h"

#include "fujinet/io/devices/network_protocol_stub.h"
#include "detail/tcp_protocol.h"

namespace fujinet::platform {

// Raw TCP is usable independently of the TLS and HTTP backends.
io::ProtocolRegistry make_default_network_registry()
{
    io::ProtocolRegistry r;
    r.register_scheme("tcp", [] { return std::make_unique<pico::TcpProtocol>(); });
    for (const char* scheme : {"tls", "http", "https"}) {
        r.register_scheme(scheme, [] { return std::make_unique<io::StubNetworkProtocol>(); });
    }
    return r;
}

} // namespace fujinet::platform
