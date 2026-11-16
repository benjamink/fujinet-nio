#pragma once
// Deterministic boundary double, not a replacement network stack. Tests drive
// callbacks explicitly; production adapter code is compiled unchanged.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>
using err_t = int;
using u16_t = std::uint16_t;
constexpr err_t ERR_OK=0, ERR_MEM=-1, ERR_BUF=-2, ERR_TIMEOUT=-3, ERR_RTE=-4,
    ERR_INPROGRESS=-5, ERR_VAL=-6, ERR_WOULDBLOCK=-7, ERR_USE=-8,
    ERR_ALREADY=-9, ERR_ISCONN=-10, ERR_CONN=-11, ERR_IF=-12,
    ERR_ABRT=-13, ERR_RST=-14, ERR_CLSD=-15, ERR_ARG=-16;
constexpr int IPADDR_TYPE_V4=0, TCP_WRITE_FLAG_COPY=1, SOF_KEEPALIVE=8,
    SOF_REUSEADDR=4, PBUF_TRANSPORT=0, PBUF_RAM=0;
struct ip_addr_t { std::uint32_t addr=0; };
inline void ip_addr_set_any(int, ip_addr_t* p) { p->addr=0; }
struct pbuf { pbuf* next=nullptr; void* payload=nullptr; u16_t len=0,tot_len=0; };
struct tcp_pcb;
using tcp_recv_fn = err_t(*)(void*,tcp_pcb*,pbuf*,err_t);
using tcp_error_fn = void(*)(void*,err_t);
using tcp_connected_fn = err_t(*)(void*,tcp_pcb*,err_t);
using tcp_accept_fn = err_t(*)(void*,tcp_pcb*,err_t);
struct tcp_pcb {
    void* arg=nullptr;
    tcp_recv_fn recv=nullptr;
    tcp_error_fn error=nullptr;
    tcp_connected_fn connected=nullptr;
    tcp_accept_fn accept=nullptr;
    ip_addr_t remote_ip{};
    u16_t remote_port=0;
    unsigned sndbuf=4096, credited=0, options=0;
    bool listening=false, dead=false, nodelay=false, shut=false;
    std::vector<std::uint8_t> sent;
};
struct udp_pcb;
using udp_recv_fn = void(*)(void*,udp_pcb*,pbuf*,const ip_addr_t*,u16_t);
struct udp_pcb {
    void* arg=nullptr; udp_recv_fn recv=nullptr;
    bool dead=false; ip_addr_t remote_ip{}; u16_t remote_port=0;
    std::vector<std::uint8_t> sent;
};
using dns_found_callback = void(*)(const char*,const ip_addr_t*,void*);
namespace fake {
inline int lock_depth=0;
inline std::uint64_t now=0;
inline std::function<void()> on_sleep;
inline std::vector<std::unique_ptr<tcp_pcb>> tcp;
inline std::vector<std::unique_ptr<udp_pcb>> udp;
inline err_t connect_result=ERR_OK,write_result=ERR_OK,close_result=ERR_OK;
inline err_t dns_result=ERR_OK;
inline unsigned freed_pbufs=0,aborts=0;
struct Dns { dns_found_callback callback; void* arg; };
inline std::vector<Dns> dns;
inline void locked() { if (!lock_depth) throw std::logic_error("unguarded raw API"); }
inline void live(tcp_pcb* p) { locked(); if(p->dead) throw std::logic_error("dead pcb"); }
inline void stream(tcp_pcb* p) { live(p); if(p->listening) throw std::logic_error("listen pcb used as stream"); }
inline void complete_dns(std::size_t i,bool success=true) {
    auto d=dns.at(i); ip_addr_t ip{0x01020304};
    ++lock_depth; d.callback("host",success?&ip:nullptr,d.arg); --lock_depth;
}
}
inline tcp_pcb* tcp_new_ip_type(int) {
    fake::locked(); fake::tcp.push_back(std::make_unique<tcp_pcb>()); return fake::tcp.back().get();
}
inline void tcp_arg(tcp_pcb* p,void* a) { fake::live(p); p->arg=a; }
inline void tcp_err(tcp_pcb* p,tcp_error_fn f) { fake::stream(p); p->error=f; }
inline void tcp_recv(tcp_pcb* p,tcp_recv_fn f) { fake::stream(p); p->recv=f; }
inline err_t tcp_connect(tcp_pcb* p,const ip_addr_t* ip,u16_t port,tcp_connected_fn f) {
    fake::stream(p); p->remote_ip=*ip;p->remote_port=port;p->connected=f;return fake::connect_result;
}
inline err_t tcp_bind(tcp_pcb* p,const ip_addr_t*,u16_t) { fake::live(p);return ERR_OK; }
inline tcp_pcb* tcp_listen_with_backlog(tcp_pcb* p,int) { fake::stream(p);p->listening=true;return p; }
inline void tcp_accept(tcp_pcb* p,tcp_accept_fn f) { fake::live(p);p->accept=f; }
inline err_t tcp_close(tcp_pcb* p) { fake::live(p); if(fake::close_result==ERR_OK)p->dead=true; return fake::close_result; }
inline void tcp_abort(tcp_pcb* p) {
    fake::live(p); p->dead=true; ++fake::aborts;
    if(p->error)p->error(p->arg,ERR_ABRT);
}
inline unsigned tcp_sndbuf(tcp_pcb* p) { fake::stream(p);return p->sndbuf; }
inline err_t tcp_write(tcp_pcb* p,const void* data,u16_t n,int) {
    fake::stream(p);
    if(fake::write_result!=ERR_OK)return fake::write_result;
    auto* b=static_cast<const std::uint8_t*>(data);p->sent.insert(p->sent.end(),b,b+n);return ERR_OK;
}
inline err_t tcp_output(tcp_pcb* p) { fake::stream(p);return ERR_OK; }
inline void tcp_recved(tcp_pcb* p,u16_t n) { fake::stream(p);p->credited+=n; }
inline err_t tcp_shutdown(tcp_pcb* p,int,int) { fake::stream(p);p->shut=true;return ERR_OK; }
inline void tcp_nagle_disable(tcp_pcb* p) { fake::stream(p);p->nodelay=true; }
inline void tcp_nagle_enable(tcp_pcb* p) { fake::stream(p);p->nodelay=false; }
inline void ip_set_option(tcp_pcb* p,int o) { fake::live(p);p->options|=o; }
inline void ip_reset_option(tcp_pcb* p,int o) { fake::live(p);p->options&=~o; }
inline pbuf* pbuf_alloc(int,u16_t n,int) {
    fake::locked();auto* p=new pbuf;p->payload=new std::uint8_t[n];p->len=p->tot_len=n;return p;
}
inline void pbuf_cat(pbuf* head,pbuf* tail) {
    pbuf* p=head;for(;p->next;p=p->next)p->tot_len=static_cast<u16_t>(p->tot_len+tail->tot_len);
    p->tot_len=static_cast<u16_t>(p->tot_len+tail->tot_len);p->next=tail;
}
inline u16_t pbuf_free(pbuf* p) {
    fake::locked();u16_t count=0;
    while(p){auto* next=p->next;delete[] static_cast<std::uint8_t*>(p->payload);delete p;p=next;++count;++fake::freed_pbufs;}
    return count;
}
inline err_t pbuf_take(pbuf* p,const void* data,u16_t n) {
    fake::locked();std::memcpy(p->payload,data,n);return ERR_OK;
}
inline u16_t pbuf_copy_partial(const pbuf* p,void* out,u16_t n,u16_t offset) {
    fake::locked();auto* dest=static_cast<std::uint8_t*>(out);u16_t done=0;
    for(;p && done<n;p=p->next){if(offset>=p->len){offset-=p->len;continue;}
        auto part=std::min<unsigned>(n-done,p->len-offset);
        std::memcpy(dest+done,static_cast<const std::uint8_t*>(p->payload)+offset,part);done+=part;offset=0;}
    return done;
}
inline udp_pcb* udp_new_ip_type(int) { fake::locked();fake::udp.push_back(std::make_unique<udp_pcb>());return fake::udp.back().get(); }
inline void udp_recv(udp_pcb* p,udp_recv_fn f,void* arg) { fake::locked();p->recv=f;p->arg=arg; }
inline void udp_remove(udp_pcb* p) { fake::locked();p->dead=true; }
inline err_t udp_connect(udp_pcb* p,const ip_addr_t* ip,u16_t port) { fake::locked();p->remote_ip=*ip;p->remote_port=port;return ERR_OK; }
inline err_t udp_sendto(udp_pcb* p,pbuf* b,const ip_addr_t*,u16_t) {
    fake::locked();p->sent.resize(b->tot_len);pbuf_copy_partial(b,p->sent.data(),b->tot_len,0);return ERR_OK;
}
inline err_t udp_send(udp_pcb* p,pbuf* b) { return udp_sendto(p,b,&p->remote_ip,p->remote_port); }
inline err_t dns_gethostbyname(const char*,ip_addr_t* ip,dns_found_callback cb,void* arg) {
    fake::locked();
    if(fake::dns_result==ERR_OK)ip->addr=0x01020304;
    if(fake::dns_result==ERR_INPROGRESS)fake::dns.push_back({cb,arg});
    return fake::dns_result;
}
