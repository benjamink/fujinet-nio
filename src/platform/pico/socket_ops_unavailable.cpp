// TCP and UDP socket operations for the Pico 2 W: NOT IMPLEMENTED YET.
//
// The Pico SDK's default lwIP build (NO_SYS=1, cyw43_arch_lwip_*_background)
// has no BSD socket API. The real backend is either lwIP raw pcbs behind these
// interfaces, or FreeRTOS plus lwIP sockets. Until then every call fails with
// ENOSYS so the rest of the core links and is measurable (README, "Sockets").
#include <cerrno>
#include <cstring>

#include "pico/time.h"

#include "fujinet/platform/tcp_socket_ops.h"
#include "fujinet/platform/udp_socket_ops.h"

namespace fujinet::net {
namespace {

class UnavailableTcp final : public ITcpSocketOps {
public:
    int socket(int, int, int) override { errno = ENOSYS; return -1; }
    void close(int) override {}
    int connect(int, const sockaddr*, SockLen) override { errno = ENOSYS; return -1; }
    int bind(int, const sockaddr*, SockLen) override { errno = ENOSYS; return -1; }
    int listen(int, int) override { errno = ENOSYS; return -1; }
    int accept(int, sockaddr*, SockLen*) override { errno = ENOSYS; return -1; }
    int set_nonblocking(int) override { errno = ENOSYS; return -1; }
    bool poll_connect_complete(int) override { errno = ENOSYS; return false; }
    SSize send(int, const void*, std::size_t) override { errno = ENOSYS; return -1; }
    SSize recv(int, void*, std::size_t) override { errno = ENOSYS; return -1; }
    int shutdown_write(int) override { errno = ENOSYS; return -1; }
    int get_so_error(int) override { return ENOSYS; }
    int setsockopt(int, int, int, const void*, SockLen) override { errno = ENOSYS; return -1; }
    void apply_stream_socket_options(int, bool, bool) override {}
    void apply_listen_socket_options(int) override {}
    int getaddrinfo(const char*, const char*, const void*, AddrInfo**) override { return ENOSYS; }
    const void* tcp_stream_addrinfo_hints() const noexcept override { return nullptr; }
    const void* tcp_stream_passive_addrinfo_hints() const noexcept override { return nullptr; }
    void freeaddrinfo(AddrInfo*) override {}
    AddrInfo* addrinfo_next(AddrInfo*) override { return nullptr; }
    int addrinfo_family(AddrInfo*) override { return 0; }
    int addrinfo_socktype(AddrInfo*) override { return 0; }
    int addrinfo_protocol(AddrInfo*) override { return 0; }
    const sockaddr* addrinfo_addr(AddrInfo*, SockLen* len) override { if (len) *len = 0; return nullptr; }
    std::uint64_t now_ms() override { return to_ms_since_boot(get_absolute_time()); }
    int last_errno() override { return errno; }
    const char* err_string(int e) override { return std::strerror(e); }
    bool is_would_block(int e) const noexcept override { return e == EAGAIN || e == EWOULDBLOCK; }
    bool is_in_progress(int e) const noexcept override { return e == EINPROGRESS; }
    bool is_peer_gone(int e) const noexcept override { return e == ECONNRESET || e == EPIPE; }
    int err_timed_out() const noexcept override { return ETIMEDOUT; }
    int err_conn_refused() const noexcept override { return ECONNREFUSED; }
    int err_host_unreach() const noexcept override { return EHOSTUNREACH; }
};

class UnavailableUdp final : public IUdpSocketOps {
public:
    int socket(int, int, int) override { errno = ENOSYS; return -1; }
    void close(int) override {}
    int connect(int, const sockaddr*, SockLen) override { errno = ENOSYS; return -1; }
    int set_nonblocking(int) override { errno = ENOSYS; return -1; }
    SSize send(int, const void*, std::size_t) override { errno = ENOSYS; return -1; }
    SSize sendto(int, const void*, std::size_t, const sockaddr*, SockLen) override { errno = ENOSYS; return -1; }
    SSize recv(int, void*, std::size_t) override { errno = ENOSYS; return -1; }
    SSize recvfrom(int, void*, std::size_t, sockaddr*, SockLen*) override { errno = ENOSYS; return -1; }
    bool poll_readable(int) override { return false; }
    bool wait_readable(int, std::chrono::milliseconds) override { return false; }
    int getaddrinfo(const char*, const char*, const void*, AddrInfo**) override { return ENOSYS; }
    const void* udp_addrinfo_hints() const noexcept override { return nullptr; }
    void freeaddrinfo(AddrInfo*) override {}
    AddrInfo* addrinfo_next(AddrInfo*) override { return nullptr; }
    int addrinfo_family(AddrInfo*) override { return 0; }
    int addrinfo_socktype(AddrInfo*) override { return 0; }
    int addrinfo_protocol(AddrInfo*) override { return 0; }
    const sockaddr* addrinfo_addr(AddrInfo*, SockLen* len) override { if (len) *len = 0; return nullptr; }
    int last_errno() override { return errno; }
    const char* err_string(int e) override { return std::strerror(e); }
    bool is_would_block(int e) const noexcept override { return e == EAGAIN || e == EWOULDBLOCK; }
};

} // namespace
} // namespace fujinet::net

namespace fujinet::platform {

net::ITcpSocketOps& default_tcp_socket_ops()
{
    static net::UnavailableTcp ops;
    return ops;
}

net::IUdpSocketOps& default_udp_socket_ops()
{
    static net::UnavailableUdp ops;
    return ops;
}

} // namespace fujinet::platform
