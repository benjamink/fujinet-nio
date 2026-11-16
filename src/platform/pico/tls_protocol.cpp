#include "detail/tls_protocol.h"
#include "detail/socket_state.h"
#include "fujinet/net/tcp_network_protocol_common.h"
#include "fujinet/net/https_trust_config.h"
#include "fujinet/net/test_ca_cert.h"
#include "fujinet/platform/time.h"
#include "ca_bundle_data.h"
#include "detail/tls_trust.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/altcp_tls.h"
#include "mbedtls/ssl.h"
#include "mbedtls/platform_time.h"
#include <algorithm>
#include <cstring>
#include <vector>
namespace fujinet::platform::pico {
int resolve_tls_address(const char*,const char*,ip_addr_t&,std::uint16_t&);
namespace {
using io::StatusCode;
struct Lock { Lock() { cyw43_arch_lwip_begin(); } ~Lock() { cyw43_arch_lwip_end(); } };
unsigned connections=0;
// The platform roots, in flash. Validated on first use; an invalid bundle trusts nothing.
const detail::CaBundle& platform_bundle() {
    static const detail::CaBundle bundle(ca_bundle,ca_bundle_size);
    return bundle;
}
mbedtls_time_t tls_time(mbedtls_time_t* out) {
    auto value=static_cast<mbedtls_time_t>(unix_time_seconds()); if(out) *out=value; return value;
}
class TlsProtocol final : public io::INetworkProtocol {
    altcp_pcb* pcb_=nullptr;
    altcp_tls_config* config_=nullptr;
    detail::TrustSources trust_;   // read by mbedTLS during the handshake: lives as long as the connection
    detail::ByteRing<16384> rx_;
    bool counted_=false, connected_=false, eof_=false;
    StatusCode error_=StatusCode::Ok;
    std::uint64_t start_=0;
    std::uint32_t read_cursor_=0, write_cursor_=0, last_read_offset_=0, last_write_offset_=0;
    std::vector<std::uint8_t> last_read_, last_write_;
    bool last_eof_=false;
    static err_t received(void* arg,altcp_pcb*,pbuf* p,err_t err) {
        auto& self=*static_cast<TlsProtocol*>(arg);
        if(!p) { self.eof_=true; return ERR_OK; }
        if(err!=ERR_OK) return err;
        if(p->tot_len>self.rx_.free()) return ERR_MEM;
        for(auto* part=p;part;part=part->next) self.rx_.push(part->payload,part->len);
        pbuf_free(p); return ERR_OK;
    }
    static void failed(void* arg,err_t) {
        auto& self=*static_cast<TlsProtocol*>(arg); self.pcb_=nullptr;
        self.error_=StatusCode::IOError; self.connected_=false;
    }
    static err_t connected(void* arg,altcp_pcb* pcb,err_t err) {
        auto& self=*static_cast<TlsProtocol*>(arg);
        auto* ssl=static_cast<mbedtls_ssl_context*>(altcp_tls_context(pcb));
        if(err!=ERR_OK || !ssl || mbedtls_ssl_get_verify_result(ssl)!=0 || !time_is_valid()) {
            self.error_=StatusCode::IOError; self.pcb_=nullptr;
            altcp_arg(pcb,nullptr); altcp_err(pcb,nullptr); altcp_abort(pcb); return ERR_ABRT;
        }
        self.connected_=true; return ERR_OK;
    }
    void release() {
        if(pcb_) {
            altcp_arg(pcb_,nullptr); altcp_recv(pcb_,nullptr); altcp_err(pcb_,nullptr);
            if(altcp_close(pcb_)!=ERR_OK) altcp_abort(pcb_);
            pcb_=nullptr;
        }
        if(config_) { altcp_tls_free_config(config_); config_=nullptr; }
        if(counted_) { --connections; counted_=false; }
    }
public:
    ~TlsProtocol() override { close(); }
    StatusCode open(const io::NetworkOpenRequest& request) override {
        close();
        if(!time_is_valid()) return StatusCode::NotReady;
        if(request.url.size()>2048 || request.url.rfind("tls://",0)!=0) return StatusCode::InvalidRequest;
        std::string host; std::uint16_t port=0; net::TcpNetworkProtocolCommon::Options options;
        if(!net::TcpNetworkProtocolCommon::parse_tcp_url("tcp"+request.url.substr(3),host,port,options) || host.empty() || host.size()>253) return StatusCode::InvalidRequest;
        ip_addr_t ip; std::uint16_t number=port;
        const int dns=resolve_tls_address(host.c_str(),std::to_string(port).c_str(),ip,number);
        if(dns) return StatusCode::IOError;
        Lock lock;
        if(connections>=2) return StatusCode::DeviceBusy;
        ++connections; counted_=true;
        mbedtls_platform_set_time(tls_time);
        // Trust is a lookup, not a parsed list: no roots go into the config, and install_trust
        // below anchors the connection in these sources.
        const auto policy=net::https_trust_policy();
        trust_={};
        if(policy!=net::HttpsTrustPolicy::TestCaOnly) {
            const auto& bundle=platform_bundle();
            if(!bundle.valid()) { release(); return StatusCode::IOError; } // fail closed
            trust_.bundle=&bundle;
        }
        if(policy!=net::HttpsTrustPolicy::PlatformDefault) {
            trust_.extra_pem=net::test_ca_cert_pem; trust_.extra_pem_len=sizeof(net::test_ca_cert_pem);
        }
        config_=altcp_tls_create_config_client(nullptr,0);
        if(!config_) { release(); return StatusCode::IOError; }
        pcb_=altcp_tls_new(config_,IPADDR_TYPE_V4);
        if(!pcb_) { release(); return StatusCode::DeviceBusy; }
        auto* ssl=static_cast<mbedtls_ssl_context*>(altcp_tls_context(pcb_));
        if(!ssl || mbedtls_ssl_set_hostname(ssl,host.c_str())!=0 || !detail::install_trust(ssl,trust_)) { release(); return StatusCode::IOError; }
        altcp_arg(pcb_,this); altcp_recv(pcb_,received); altcp_err(pcb_,failed);
        start_=time_us_64()/1000;
        if(altcp_connect(pcb_,&ip,number,connected)!=ERR_OK) { release(); return StatusCode::IOError; }
        return StatusCode::Ok;
    }
    void poll() override {
        Lock lock;
        if(pcb_ && !connected_ && time_us_64()/1000-start_>=15000) error_=StatusCode::Timeout;
        if(error_!=StatusCode::Ok) release();
    }
    StatusCode write_body(std::uint32_t offset,const std::uint8_t* data,std::size_t length,std::uint16_t& written) override {
        written=0;
        if(length && !data) return StatusCode::InvalidRequest;
        Lock lock;
        if(offset==last_write_offset_ && !last_write_.empty() && length>=last_write_.size() &&
           std::memcmp(data,last_write_.data(),last_write_.size())==0) { written=last_write_.size(); return StatusCode::Ok; }
        if(offset!=write_cursor_) return StatusCode::InvalidRequest;
        if(error_!=StatusCode::Ok) return error_;
        if(!pcb_ || !connected_) return StatusCode::NotReady;
        if(!length) return StatusCode::Ok;
        const auto n=std::min({length,std::size_t(4096),std::size_t(altcp_sndbuf(pcb_))});
        if(!n) return StatusCode::NotReady;
        if(write_cursor_>UINT32_MAX-n) return StatusCode::InvalidRequest;
        const auto err=altcp_write(pcb_,data,n,TCP_WRITE_FLAG_COPY);
        if(err==ERR_MEM || err==ERR_BUF) return StatusCode::NotReady;
        if(err!=ERR_OK) { error_=StatusCode::IOError; return error_; }
        altcp_output(pcb_); last_write_.assign(data,data+n); last_write_offset_=offset;
        written=n; write_cursor_+=n; return StatusCode::Ok;
    }
    StatusCode read_body(std::uint32_t offset,std::uint8_t* out,std::size_t length,std::uint16_t& count,bool& eof,bool& more) override {
        count=0; eof=false; more=false;
        if(length && !out) return StatusCode::InvalidRequest;
        Lock lock;
        if(offset==last_read_offset_ && !last_read_.empty() && length>=last_read_.size()) {
            count=last_read_.size(); std::memcpy(out,last_read_.data(),count); eof=last_eof_; more=rx_.size()!=0; return StatusCode::Ok;
        }
        if(offset!=read_cursor_) return StatusCode::InvalidRequest;
        if(rx_.size()) {
            const auto wanted=std::min({length,std::size_t(4096),rx_.size()});
            if(read_cursor_>UINT32_MAX-wanted) return StatusCode::InvalidRequest;
            count=rx_.pop(out,wanted);
            if(pcb_) altcp_recved(pcb_,count);
            eof=eof_ && !rx_.size(); more=rx_.size()!=0;
            last_read_.assign(out,out+count); last_read_offset_=offset; last_eof_=eof; read_cursor_+=count;
            return StatusCode::Ok;
        }
        if(error_!=StatusCode::Ok) return error_;
        eof=eof_; return eof_ ? StatusCode::Ok : StatusCode::NotReady;
    }
    StatusCode info(io::NetworkInfo& out) override { Lock lock; out={}; return error_; }
    void close() override {
        Lock lock; release(); connected_=false; eof_=false; error_=StatusCode::Ok; rx_.clear();
        read_cursor_=write_cursor_=last_read_offset_=last_write_offset_=0;
        last_read_.clear(); last_write_.clear(); last_eof_=false;
    }
    bool read_ready() const override { Lock lock; return rx_.size() || eof_ || error_!=StatusCode::Ok; }
    bool read_available(std::size_t& out) const override { Lock lock; out=rx_.size(); return true; }
    bool is_streaming() const override { return true; }
    bool requires_sequential_read() const override { return true; }
    bool requires_sequential_write() const override { return true; }
};
}
std::unique_ptr<io::INetworkProtocol> make_tls_protocol() { return std::make_unique<TlsProtocol>(); }
}
