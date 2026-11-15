#include "doctest.h"
#include <fujinet/io/core/channel.h>
#include <fujinet/platform/udp_socket_ops.h>
#include <fujinet/net/udp_channel.h>

#include <string>

using namespace fujinet;

TEST_CASE("UDP Channel - create") {
    auto& socket_ops = platform::default_udp_socket_ops();
    auto channel = std::make_unique<net::UdpChannel>(socket_ops, "localhost", 12345);
    
    // Check that the channel was created successfully
    CHECK(channel != nullptr);
}

TEST_CASE("UDP Channel - available initially false") {
    auto& socket_ops = platform::default_udp_socket_ops();
    auto channel = std::make_unique<net::UdpChannel>(socket_ops, "localhost", 12345);
    
    CHECK(!channel->available());
}

#include "doctest.h"
#include "fujinet/net/udp_channel.h"
#include "fujinet/platform/posix/udp_socket_ops_posix.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <array>

TEST_CASE("UDP channel retains its opaque peer and preserves datagram boundaries") {
    const int peer = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(peer >= 0);
    struct Cleanup { int fd; ~Cleanup() { ::close(fd); } } cleanup{peer};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(peer, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    REQUIRE(::getsockname(peer, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    timeval timeout{1, 0};
    REQUIRE(::setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    fujinet::net::UdpChannel channel(fujinet::net::get_posix_udp_socket_ops(), "localhost", ntohs(addr.sin_port));
    const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};
    channel.write(payload.data(), payload.size());
    sockaddr_in client{};
    len = sizeof(client);
    std::array<std::uint8_t, 4> got{};
    REQUIRE(::recvfrom(peer, got.data(), got.size(), 0, reinterpret_cast<sockaddr*>(&client), &len) == 4);
    CHECK(got == payload);
    REQUIRE(::sendto(peer, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr*>(&client), len) == 4);
    REQUIRE(channel.wait_for_readable(std::chrono::milliseconds(100)));
    CHECK(channel.read(got.data(), 2) == 2);
    CHECK_FALSE(channel.available()); // Truncated remainder is discarded.
    REQUIRE(::sendto(peer, payload.data(), 0, 0, reinterpret_cast<sockaddr*>(&client), len) == 0);
    REQUIRE(channel.wait_for_readable(std::chrono::milliseconds(100)));
    CHECK(channel.read(got.data(), got.size()) == 0);
    CHECK_FALSE(channel.available());
}
