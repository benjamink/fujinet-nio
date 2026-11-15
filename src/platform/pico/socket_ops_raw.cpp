// Raw NO_SYS lwIP adapter. Foreground access is serialized with the background
// CYW43 dispatcher; callbacks already run under that same lwIP lock.
#include "fujinet/platform/tcp_socket_ops.h"
#include "fujinet/platform/udp_socket_ops.h"
#include "detail/socket_state.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>

namespace fujinet::net {
// Deliberately private address representation; shared code only carries pointers.
struct AddrInfo { ip_addr_t ip{}; std::uint16_t port = 0; int type = 1; };
namespace {
using namespace platform::pico::detail;
struct Lock { Lock() { cyw43_arch_lwip_begin(); } ~Lock() { cyw43_arch_lwip_end(); } };
int fail(int e) { errno = e; return -1; }
int native_error(err_t e) {
    switch (e) {
    case ERR_OK: return 0;
    case ERR_MEM: case ERR_BUF: return EAGAIN;
    case ERR_TIMEOUT: return ETIMEDOUT;
    case ERR_RTE: return EHOSTUNREACH;
    case ERR_USE: return EADDRINUSE;
    case ERR_ABRT: return ECONNABORTED;
    case ERR_RST: return ECONNRESET;
    case ERR_CLSD: return EPIPE;
    case ERR_CONN: return ENOTCONN;
    default: return EIO;
    }
}
int result(err_t e) { return e == ERR_OK ? 0 : fail(native_error(e)); }
const AddrInfo* address(const sockaddr* addr, SockLen len) {
    if (!addr || len != sizeof(AddrInfo)) { errno = EINVAL; return nullptr; }
    return reinterpret_cast<const AddrInfo*>(addr);
}
struct DnsRequest {
    bool used = false, done = false, abandoned = false, found = false;
    ip_addr_t ip{};
};
std::array<DnsRequest, 4> dns_requests{};
void dns_done(const char*, const ip_addr_t* ip, void* arg) {
    auto& r = *static_cast<DnsRequest*>(arg);
    if (r.abandoned) { r = {}; return; }
    r.found = ip != nullptr;
    if (ip) r.ip = *ip;
    r.done = true;
}
int resolve(const char* host, const char* port, int type, AddrInfo** out) {
    if (!out) return EINVAL;
    *out = nullptr;
    char* end = nullptr;
    const auto number = port ? std::strtoul(port, &end, 10) : 0;
    if (!port || end == port || *end || number > 65535) return EINVAL;
    AddrInfo a{}; a.port = number; a.type = type;
    if (!host || !*host) { ip_addr_set_any(IPADDR_TYPE_V4, &a.ip); }
    else {
        DnsRequest* request = nullptr;
        {
            Lock lock;
            for (auto& r : dns_requests) if (!r.used) { r = {}; r.used = true; request = &r; break; }
            if (!request) return EAGAIN;
            const auto err = dns_gethostbyname(host, &request->ip, dns_done, request);
            if (err == ERR_OK) { request->done = true; request->found = true; }
            else if (err != ERR_INPROGRESS) { *request = {}; return EHOSTUNREACH; }
        }
        const auto deadline = make_timeout_time_ms(5000);
        for (;;) {
            {
                Lock lock;
                if (request->done) {
                    const bool found = request->found;
                    a.ip = request->ip;
                    *request = {};
                    if (!found) return EHOSTUNREACH;
                    break;
                }
                if (time_reached(deadline)) {
                    // Callback owns this stable slot until completion, never stack/heap state.
                    request->abandoned = true;
                    return ETIMEDOUT;
                }
            }
            sleep_ms(1);
        }
    }
    *out = new (std::nothrow) AddrInfo(a);
    return *out ? 0 : ENOMEM;
}
struct TcpState {
    tcp_pcb* pcb = nullptr;
    ByteRing<4096> rx;
    bool connected = false, eof = false, listening = false;
    int error = 0;
    std::array<int, 4> accepted{{-1, -1, -1, -1}};
};
HandleTable<TcpState, 8> tcp_states;
void tcp_error(void* arg, err_t e) {
    auto& s = *static_cast<TcpState*>(arg);
    s.pcb = nullptr; s.error = native_error(e); s.connected = false;
}
err_t tcp_received(void* arg, tcp_pcb*, pbuf* p, err_t e) {
    auto& s = *static_cast<TcpState*>(arg);
    if (!p) { s.eof = true; return ERR_OK; }
    if (e != ERR_OK) return e;
    if (p->tot_len > s.rx.free()) return ERR_MEM; // lwIP retains packet and retries.
    for (auto* part = p; part; part = part->next) s.rx.push(part->payload, part->len);
    pbuf_free(p);
    // tcp_recved happens only when the application consumes bytes (backpressure).
    return ERR_OK;
}
void setup(TcpState& s) {
    tcp_arg(s.pcb, &s); tcp_err(s.pcb, tcp_error); tcp_recv(s.pcb, tcp_received);
}
err_t tcp_connected(void* arg, tcp_pcb*, err_t e) {
    auto& s = *static_cast<TcpState*>(arg);
    s.connected = e == ERR_OK; s.error = native_error(e); return ERR_OK;
}
err_t on_tcp_accept(void* arg, tcp_pcb* pcb, err_t e) {
    auto& listener = *static_cast<TcpState*>(arg);
    for (auto& pending : listener.accepted) if (pending < 0 && e == ERR_OK) {
        const int fd = tcp_states.allocate();
        if (fd < 0) break;
        auto& s = *tcp_states.get(fd); s.pcb = pcb; s.connected = true;
        setup(s); pending = fd; return ERR_OK;
    }
    tcp_abort(pcb); return ERR_ABRT;
}
void close_tcp(int fd) {
    auto* s = tcp_states.get(fd);
    if (!s) return;
    for (int pending : s->accepted) if (pending >= 0) close_tcp(pending);
    if (s->pcb) {
        tcp_arg(s->pcb, nullptr);
        if (s->listening) tcp_accept(s->pcb, nullptr);
        else { tcp_recv(s->pcb, nullptr); tcp_err(s->pcb, nullptr); }
        // Detach callbacks before releasing the slot, including on ERR_MEM.
        if (tcp_close(s->pcb) != ERR_OK) tcp_abort(s->pcb);
    }
    tcp_states.release(fd);
}
#define ADDRESS_OPS(TYPE) \
    int getaddrinfo(const char* h, const char* p, const void*, AddrInfo** o) override { return resolve(h,p,TYPE,o); } \
    void freeaddrinfo(AddrInfo* a) override { delete a; } \
    AddrInfo* addrinfo_next(AddrInfo*) override { return nullptr; } \
    int addrinfo_family(AddrInfo*) override { return 4; } \
    int addrinfo_socktype(AddrInfo* a) override { return a->type; } \
    int addrinfo_protocol(AddrInfo* a) override { return a->type == 1 ? 6 : 17; } \
    const sockaddr* addrinfo_addr(AddrInfo* a, SockLen* n) override { if(n) *n=sizeof(*a); return reinterpret_cast<const sockaddr*>(a); } \
    int last_errno() override { return errno; } \
    const char* err_string(int e) override { return std::strerror(e); } \
    bool is_would_block(int e) const noexcept override { return e == EAGAIN || e == EWOULDBLOCK; }
class RawTcp final : public ITcpSocketOps {
public:
    int socket(int family, int type, int protocol) override {
        Lock lock;
        if (family != 4 || type != 1 || protocol != 6) return fail(EINVAL);
        int fd = tcp_states.allocate();
        if (fd < 0) return fail(EMFILE);
        auto& s = *tcp_states.get(fd); s.pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!s.pcb) { tcp_states.release(fd); return fail(ENOMEM); }
        setup(s); return fd;
    }
    void close(int fd) override { Lock lock; close_tcp(fd); }
    int connect(int fd, const sockaddr* addr, SockLen n) override {
        Lock lock; auto* s = tcp_states.get(fd); auto* a = address(addr,n);
        if (!s || !s->pcb || s->listening || !a) return fail(EINVAL);
        const auto e = tcp_connect(s->pcb, &a->ip, a->port, tcp_connected);
        return e == ERR_OK ? fail(EINPROGRESS) : result(e);
    }
    int bind(int fd, const sockaddr* addr, SockLen n) override {
        Lock lock; auto* s = tcp_states.get(fd); auto* a = address(addr,n);
        return s && s->pcb && !s->listening && a ? result(tcp_bind(s->pcb, &a->ip, a->port)) : fail(EINVAL);
    }
    int listen(int fd, int backlog) override {
        Lock lock; auto* s = tcp_states.get(fd);
        if (!s || !s->pcb || s->listening) return fail(EINVAL);
        auto* pcb = tcp_listen_with_backlog(s->pcb, std::clamp(backlog, 1, 4));
        if (!pcb) return fail(ENOMEM);
        s->pcb = pcb; s->listening = true; tcp_arg(pcb,s); tcp_accept(pcb,on_tcp_accept); return 0;
    }
    int accept(int fd, sockaddr* addr, SockLen* n) override {
        Lock lock; auto* s = tcp_states.get(fd);
        if (!s || !s->listening) return fail(EINVAL);
        for (auto& pending : s->accepted) if (pending >= 0) {
            const int accepted = pending; pending = -1;
            auto* child = tcp_states.get(accepted);
            if (addr && n && child && child->pcb) {
                AddrInfo a{child->pcb->remote_ip, child->pcb->remote_port, 1};
                std::memcpy(addr, &a, std::min<std::size_t>(*n,sizeof(a))); *n=sizeof(a);
            }
            return accepted;
        }
        return fail(EAGAIN);
    }
    int set_nonblocking(int fd) override { Lock lock; return tcp_states.get(fd) ? 0 : fail(EBADF); }
    bool poll_connect_complete(int fd) override {
        Lock lock; auto* s=tcp_states.get(fd);
        if (!s) { errno=EBADF; return false; }
        errno=s->error; return s->connected || s->error;
    }
    SSize send(int fd, const void* data, std::size_t n) override {
        Lock lock; auto* s=tcp_states.get(fd);
        if (!s || s->listening) return fail(EBADF);
        if (s->error) return fail(s->error);
        if (!s->pcb || !s->connected) return fail(ENOTCONN);
        if (!n) return 0;
        n=std::min<std::size_t>(n,tcp_sndbuf(s->pcb));
        if (!n) return fail(EAGAIN);
        const auto e=tcp_write(s->pcb,data,n,TCP_WRITE_FLAG_COPY);
        if (e != ERR_OK) return fail(native_error(e));
        tcp_output(s->pcb); return n;
    }
    SSize recv(int fd, void* out, std::size_t n) override {
        Lock lock; auto* s=tcp_states.get(fd);
        if (!s || s->listening) return fail(EBADF);
        if (!n) return 0;
        if (s->rx.size()) { n=s->rx.pop(out,n); if(s->pcb) tcp_recved(s->pcb,n); return n; }
        if (s->error) return fail(s->error);
        return s->eof ? 0 : fail(EAGAIN);
    }
    int shutdown_write(int fd) override { Lock lock; auto* s=tcp_states.get(fd); return s && s->pcb && !s->listening ? result(tcp_shutdown(s->pcb,0,1)) : fail(EBADF); }
    int get_so_error(int fd) override { Lock lock; auto* s=tcp_states.get(fd); return s ? s->error : EBADF; }
    int setsockopt(int, int, int, const void*, SockLen) override { return fail(ENOPROTOOPT); }
    void apply_stream_socket_options(int fd, bool nodelay, bool keepalive) override {
        Lock lock; auto* s=tcp_states.get(fd); if (!s || !s->pcb || s->listening) return;
        if (nodelay) tcp_nagle_disable(s->pcb); else tcp_nagle_enable(s->pcb);
        if (keepalive) ip_set_option(s->pcb,SOF_KEEPALIVE); else ip_reset_option(s->pcb,SOF_KEEPALIVE);
    }
    void apply_listen_socket_options(int fd) override { Lock lock; auto* s=tcp_states.get(fd); if(s && s->pcb) ip_set_option(s->pcb,SOF_REUSEADDR); }
    const void* tcp_stream_addrinfo_hints() const noexcept override { return nullptr; }
    const void* tcp_stream_passive_addrinfo_hints() const noexcept override { return nullptr; }
    std::uint64_t now_ms() override { return to_ms_since_boot(get_absolute_time()); }
    bool is_in_progress(int e) const noexcept override { return e == EINPROGRESS; }
    bool is_peer_gone(int e) const noexcept override { return e == ECONNRESET || e == EPIPE; }
    int err_timed_out() const noexcept override { return ETIMEDOUT; }
    int err_conn_refused() const noexcept override { return ECONNREFUSED; }
    int err_host_unreach() const noexcept override { return EHOSTUNREACH; }
    ADDRESS_OPS(1)
};
struct UdpState {
    udp_pcb* pcb = nullptr;
    DatagramQueue<1472,4> rx;
    std::array<AddrInfo,4> peers{};
    unsigned head=0, count=0;
};
HandleTable<UdpState,4> udp_states;
void udp_received(void* arg, udp_pcb*, pbuf* p, const ip_addr_t* ip, u16_t port) {
    auto& s=*static_cast<UdpState*>(arg);
    if (p->tot_len <= 1472 && !s.rx.full()) {
        s.rx.emplace(p->tot_len, [p](void* out, std::size_t n) { pbuf_copy_partial(p,out,n,0); });
        s.peers[(s.head+s.count)%4] = AddrInfo{*ip,port,2}; ++s.count;
    }
    pbuf_free(p);
}
class RawUdp final : public IUdpSocketOps {
public:
    int socket(int family,int type,int protocol) override {
        Lock lock; if(family!=4 || type!=2 || protocol!=17) return fail(EINVAL);
        const int fd=udp_states.allocate(); if(fd<0) return fail(EMFILE);
        auto& s=*udp_states.get(fd); s.pcb=udp_new_ip_type(IPADDR_TYPE_V4);
        if(!s.pcb) { udp_states.release(fd); return fail(ENOMEM); }
        udp_recv(s.pcb,udp_received,&s); return fd;
    }
    void close(int fd) override { Lock lock; auto* s=udp_states.get(fd); if(s) { udp_remove(s->pcb); udp_states.release(fd); } }
    int connect(int fd,const sockaddr* addr,SockLen n) override {
        Lock lock; auto* s=udp_states.get(fd); auto* a=address(addr,n);
        return s && a ? result(udp_connect(s->pcb,&a->ip,a->port)) : fail(EINVAL);
    }
    int set_nonblocking(int fd) override { Lock lock; return udp_states.get(fd) ? 0 : fail(EBADF); }
    SSize sendto(int fd,const void* data,std::size_t n,const sockaddr* addr,SockLen len) override {
        Lock lock; auto* s=udp_states.get(fd); if(!s) return fail(EBADF);
        if(n>1472) return fail(EMSGSIZE);
        auto* a=addr ? address(addr,len) : nullptr; if(addr && !a) return -1;
        pbuf* p=pbuf_alloc(PBUF_TRANSPORT,n,PBUF_RAM); if(!p) return fail(EAGAIN);
        if(n) pbuf_take(p,data,n);
        const auto e=a ? udp_sendto(s->pcb,p,&a->ip,a->port) : udp_send(s->pcb,p);
        pbuf_free(p); return e==ERR_OK ? static_cast<SSize>(n) : fail(native_error(e));
    }
    SSize send(int fd,const void* p,std::size_t n) override { return sendto(fd,p,n,nullptr,0); }
    SSize recvfrom(int fd,void* out,std::size_t n,sockaddr* addr,SockLen* len) override {
        Lock lock; auto* s=udp_states.get(fd); if(!s) return fail(EBADF);
        if(s->rx.empty()) return fail(EAGAIN);
        if(addr && len) { const auto& a=s->peers[s->head]; std::memcpy(addr,&a,std::min<std::size_t>(*len,sizeof(a))); *len=sizeof(a); }
        s->head=(s->head+1)%4; --s->count; return s->rx.pop(out,n);
    }
    SSize recv(int fd,void* p,std::size_t n) override { return recvfrom(fd,p,n,nullptr,nullptr); }
    bool poll_readable(int fd) override { Lock lock; auto* s=udp_states.get(fd); return s && !s->rx.empty(); }
    bool wait_readable(int fd,std::chrono::milliseconds timeout) override {
        const auto deadline=make_timeout_time_ms(std::max<std::int64_t>(0,timeout.count()));
        do { if(poll_readable(fd)) return true; if(time_reached(deadline)) return false; sleep_ms(1); } while(true);
    }
    const void* udp_addrinfo_hints() const noexcept override { return nullptr; }
    ADDRESS_OPS(2)
};
#undef ADDRESS_OPS
}
}
namespace fujinet::platform {
net::ITcpSocketOps& default_tcp_socket_ops() { static net::RawTcp ops; return ops; }
net::IUdpSocketOps& default_udp_socket_ops() { static net::RawUdp ops; return ops; }
}
