#pragma once
#include "fujinet/io/devices/network_protocol.h"
#include <functional>
#include <memory>
namespace fujinet::platform::pico {
using HttpStreamFactory=std::function<std::unique_ptr<io::INetworkProtocol>(bool tls)>;
std::unique_ptr<io::INetworkProtocol> make_http_protocol(HttpStreamFactory factory,std::function<std::uint64_t()> now_ms);
}
