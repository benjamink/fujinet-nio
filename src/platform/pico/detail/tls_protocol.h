#pragma once
#include "fujinet/io/devices/network_protocol.h"
#include <memory>
namespace fujinet::platform::pico {
std::unique_ptr<io::INetworkProtocol> make_tls_protocol();
}
