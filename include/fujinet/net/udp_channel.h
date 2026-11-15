#pragma once

#include "fujinet/io/core/channel.h"
#include "fujinet/net/udp_socket_ops.h"

#include <string>
#include <memory>
#include <cstddef>

namespace fujinet::net {

// Common UDP channel implementation that uses the platform-agnostic socket operations interface.
class UdpChannel final : public fujinet::io::Channel {
public:
    explicit UdpChannel(IUdpSocketOps& socket_ops, const std::string& host, uint16_t port);
    ~UdpChannel() override;
    UdpChannel(const UdpChannel&) = delete;
    UdpChannel& operator=(const UdpChannel&) = delete;

    bool available() override;
    std::size_t read(std::uint8_t* buffer, std::size_t max_len) override;
    void write(const std::uint8_t* buffer, std::size_t len) override;
    bool supports_readable_wait() const override { return true; }
    std::size_t receive_datagram_capacity() const override { return socket_ops_.recv_queue_depth(); }
    bool wait_for_readable(std::chrono::milliseconds timeout) override;

private:
    IUdpSocketOps& socket_ops_;
    std::string host_;
    uint16_t port_;
    int socket_fd_;
    bool connected_;
    AddrInfo* peer_addresses_ = nullptr;
    const sockaddr* peer_addr_ = nullptr;
    SockLen peer_addr_len_ = 0;
};

} // namespace fujinet::net
