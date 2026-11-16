#pragma once
#include "fake_lwip.h"
#include "mbedtls/ssl.h"
#include <string>
struct altcp_tls_config {};
struct altcp_pcb {
    void* arg=nullptr;
    err_t(*recv)(void*,altcp_pcb*,pbuf*,err_t)=nullptr;
    void(*error)(void*,err_t)=nullptr;
    err_t(*connected)(void*,altcp_pcb*,err_t)=nullptr;
    mbedtls_ssl_context ssl;
    bool dead=false; unsigned credit=0;
    std::vector<std::uint8_t> sent;
};
namespace tls_fake {
inline std::vector<std::unique_ptr<altcp_pcb>> pcbs;
inline bool config_had_roots=false;           // trust no longer rides on the config
inline bool trust_bundle=false,trust_extra=false; // what install_trust was given
inline unsigned installs=0;
inline unsigned configs=0;
inline bool valid_time=true,close_mem=false;
}
inline altcp_tls_config* altcp_tls_create_config_client(const std::uint8_t* p,std::size_t n) {
    tls_fake::config_had_roots=(p!=nullptr || n!=0); ++tls_fake::configs;
    return new altcp_tls_config{};
}
inline void altcp_tls_free_config(altcp_tls_config* p) { --tls_fake::configs; delete p; }
inline altcp_pcb* altcp_tls_new(altcp_tls_config*,int) { tls_fake::pcbs.push_back(std::make_unique<altcp_pcb>()); return tls_fake::pcbs.back().get(); }
inline void* altcp_tls_context(altcp_pcb* p) { return &p->ssl; }
inline void altcp_arg(altcp_pcb* p,void* a) { p->arg=a; }
inline void altcp_recv(altcp_pcb* p,decltype(altcp_pcb::recv) f) { p->recv=f; }
inline void altcp_err(altcp_pcb* p,decltype(altcp_pcb::error) f) { p->error=f; }
inline err_t altcp_connect(altcp_pcb* p,const ip_addr_t*,std::uint16_t,decltype(altcp_pcb::connected) f) { p->connected=f; return ERR_OK; }
inline err_t altcp_close(altcp_pcb* p) { if(tls_fake::close_mem) return ERR_MEM; p->dead=true; return ERR_OK; }
inline void altcp_abort(altcp_pcb* p) { p->dead=true; if(p->error) p->error(p->arg,ERR_ABRT); }
inline unsigned altcp_sndbuf(altcp_pcb*) { return 17; }
inline err_t altcp_write(altcp_pcb* p,const void* data,std::size_t n,int) { auto* b=static_cast<const std::uint8_t*>(data);p->sent.insert(p->sent.end(),b,b+n);return ERR_OK; }
inline void altcp_output(altcp_pcb*) {}
inline void altcp_recved(altcp_pcb* p,unsigned n) { p->credit+=n; }
