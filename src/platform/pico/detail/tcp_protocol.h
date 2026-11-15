#pragma once
#include "fujinet/net/tcp_network_protocol_common.h"
#include "fujinet/platform/tcp_socket_ops.h"
namespace fujinet::platform::pico {
class TcpProtocol final : public io::INetworkProtocol {
    net::TcpNetworkProtocolCommon common_{default_tcp_socket_ops()};
public:
    io::StatusCode open(const io::NetworkOpenRequest& r) override { return common_.open(r); }
    io::StatusCode write_body(std::uint32_t o,const std::uint8_t* p,std::size_t n,std::uint16_t& w) override { return common_.write_body(o,p,n,w); }
    io::StatusCode read_body(std::uint32_t o,std::uint8_t* p,std::size_t n,std::uint16_t& r,bool& e,bool& m) override { return common_.read_body(o,p,n,r,e,m); }
    io::StatusCode info(io::NetworkInfo& i) override { return common_.info(i); }
    void poll() override { common_.poll(); }
    void close() override { common_.close(); }
    bool read_ready() const override { return common_.read_ready(); }
    bool read_available(std::size_t& n) const override { n=common_.read_available(); return true; }
    bool is_streaming() const override { return true; }
    bool requires_sequential_read() const override { return true; }
    bool requires_sequential_write() const override { return true; }
};
}
