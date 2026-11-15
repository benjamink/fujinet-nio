#include "fujinet/platform/network_registry.h"

#include "detail/tcp_protocol.h"
#include "detail/tls_protocol.h"
#include "detail/http_protocol.h"
#include "pico/time.h"

namespace fujinet::platform {

// Raw TCP is usable independently of the TLS and HTTP backends.
io::ProtocolRegistry make_default_network_registry()
{
    io::ProtocolRegistry r;
    r.register_scheme("tcp", [] { return std::make_unique<pico::TcpProtocol>(); });
    r.register_scheme("tls", [] { return pico::make_tls_protocol(); });
    for (const char* scheme : {"http", "https"}) {
        r.register_scheme(scheme, [] {
            return pico::make_http_protocol([](bool tls)->std::unique_ptr<io::INetworkProtocol> {
                if(tls) return pico::make_tls_protocol();
                return std::make_unique<pico::TcpProtocol>();
            }, [] { return time_us_64()/1000; });
        });
    }
    return r;
}

} // namespace fujinet::platform
