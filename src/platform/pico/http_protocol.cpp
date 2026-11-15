#include "detail/http_protocol.h"
#include "detail/http_parser.h"
#include "detail/socket_state.h"
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
namespace fujinet::platform::pico {
namespace {
using io::StatusCode;
class HttpProtocol final : public io::INetworkProtocol {
    HttpStreamFactory factory_;
    std::function<std::uint64_t()> now_;
    std::unique_ptr<io::INetworkProtocol> stream_;
    io::NetworkOpenRequest request_;
    detail::HttpUrl url_;
    detail::HttpParser parser_;
    detail::ByteRing<8192> body_;
    std::string request_headers_;
    std::size_t headers_sent_=0;
    std::string upload_chunk_;
    std::size_t chunk_sent_=0;
    bool unknown_upload_=false, upload_committed_=false;
    bool commit_acknowledged_=false;
    std::uint32_t commit_offset_=0;
    std::array<std::uint8_t,512> pending_{};
    std::size_t pending_at_=0,pending_size_=0;
    bool wire_eof_=false,headers_handled_=false;
    std::uint32_t wire_read_=0,wire_write_=0,body_write_=0,body_read_=0;
    std::uint32_t last_read_offset_=0,last_write_offset_=0;
    std::vector<std::uint8_t> last_read_,last_write_;
    bool last_eof_=false;
    unsigned redirects_=0;
    std::uint64_t progress_=0;
    StatusCode error_=StatusCode::Ok;
    bool failed(StatusCode status) { error_=status; if(stream_) stream_->close(); return false; }
    StatusCode start() {
        if(!detail::build_http_request(request_,url_,request_headers_)) return StatusCode::InvalidRequest;
        parser_.reset(request_.method==5); body_.clear(); pending_at_=pending_size_=0;
        upload_chunk_.clear(); chunk_sent_=0; upload_committed_=false;
        unknown_upload_=(request_.flags&4) && !request_.bodyLenHint && (request_.method==2 || request_.method==3);
        headers_sent_=0; wire_eof_=headers_handled_=false;
        wire_read_=wire_write_=body_write_=0;
        stream_=factory_(url_.tls); if(!stream_) return StatusCode::Unsupported;
        io::NetworkOpenRequest stream_request;
        stream_request.url=std::string(url_.tls ? "tls://" : "tcp://")+url_.host+":"+std::to_string(url_.port);
        progress_=now_(); return stream_->open(stream_request);
    }
    bool headers() {
        headers_handled_=true;
        const auto status=parser_.status();
        if(!(request_.flags&2) || (status!=301 && status!=302 && status!=303 && status!=307 && status!=308)) return true;
        std::string location;
        for(const auto& [key,value]:parser_.headers()) if(key=="location") { location=value; break; }
        if(location.empty()) return true;
        if(redirects_++>=3) return failed(StatusCode::IOError);
        const auto next=detail::redirect_url(url_,location);
        detail::HttpUrl target;
        if(!detail::parse_http_url(next,target) || (url_.tls && !target.tls)) return failed(StatusCode::IOError);
        const bool to_get=(status==303 && request_.method!=5) || ((status==301 || status==302) && request_.method==2);
        if(!to_get && (request_.bodyLenHint || body_write_)) return failed(StatusCode::Unsupported); // no unbounded upload replay buffer
        const bool cross_origin=url_.tls!=target.tls || url_.host!=target.host || url_.port!=target.port;
        request_.headers.erase(std::remove_if(request_.headers.begin(),request_.headers.end(),[&](const auto& header) {
            const auto key=detail::http_request_lower(header.first);
            return (cross_origin && (key=="authorization" || key=="proxy-authorization" || key=="cookie")) || (to_get && key=="content-length");
        }),request_.headers.end());
        if(to_get) { request_.method=1; request_.bodyLenHint=0; request_.flags&=~4; }
        request_.url=next; stream_->close();
        const auto result=start(); if(result!=StatusCode::Ok) return failed(result);
        return false; // abandon all bytes from the old response before returning to poll
    }
public:
    HttpProtocol(HttpStreamFactory factory,std::function<std::uint64_t()> now):factory_(std::move(factory)),now_(std::move(now)) {}
    ~HttpProtocol() override { close(); }
    StatusCode open(const io::NetworkOpenRequest& request) override {
        close();
        if(request.responseHeaderNamesLower.size()>64) return StatusCode::InvalidRequest;
        std::size_t size=0;
        for(const auto& name:request.responseHeaderNamesLower) { size+=name.size(); if(size>2048 || !detail::http_request_token(name)) return StatusCode::InvalidRequest; }
        // Check bounds before copying any client-provided strings.
        detail::HttpUrl check_url; std::string check_headers;
        if(!detail::build_http_request(request,check_url,check_headers)) return StatusCode::InvalidRequest;
        request_=request;
        const auto result=start(); if(result!=StatusCode::Ok) failed(result);
        return result;
    }
    void poll() override {
        if(!stream_ || error_!=StatusCode::Ok || parser_.done()) return;
        if(!body_.free()) { progress_=now_(); return; }
        // A caller may pause between upload blocks. Only server/network waiting
        // consumes the inactivity budget; bounded client-side upload state does not.
        if(!parser_.headers_ready() && headers_sent_==request_headers_.size() && upload_chunk_.empty() &&
           ((unknown_upload_ && !upload_committed_) || (!unknown_upload_ && body_write_<request_.bodyLenHint))) progress_=now_();
        stream_->poll();
        if(headers_sent_<request_headers_.size()) {
            std::uint16_t sent=0;
            const auto status=stream_->write_body(wire_write_,reinterpret_cast<const std::uint8_t*>(request_headers_.data()+headers_sent_),request_headers_.size()-headers_sent_,sent);
            if(status!=StatusCode::Ok && status!=StatusCode::NotReady && status!=StatusCode::DeviceBusy) { failed(status); return; }
            headers_sent_+=sent; wire_write_+=sent; if(sent) progress_=now_();
        }
        if(headers_sent_==request_headers_.size() && chunk_sent_<upload_chunk_.size()) {
            std::uint16_t sent=0;
            const auto left=upload_chunk_.size()-chunk_sent_;
            if(wire_write_>UINT32_MAX-left) { failed(StatusCode::InvalidRequest); return; }
            const auto status=stream_->write_body(wire_write_,reinterpret_cast<const std::uint8_t*>(upload_chunk_.data()+chunk_sent_),left,sent);
            if(status!=StatusCode::Ok && status!=StatusCode::NotReady && status!=StatusCode::DeviceBusy) { failed(status); return; }
            chunk_sent_+=sent; wire_write_+=sent; if(sent) progress_=now_();
            if(chunk_sent_==upload_chunk_.size()) { upload_chunk_.clear(); chunk_sent_=0; }
        }
        // At most one received packet's worth per poll; blocked application reads
        // exert backpressure all the way through TCP without growing storage.
        for(unsigned budget=0;budget<512 && body_.free();++budget) {
            if(pending_at_==pending_size_) {
                pending_at_=pending_size_=0;
                if(wire_eof_) {
                    if(!parser_.finish()) failed(StatusCode::IOError);
                    else stream_->close();
                    return;
                }
                std::uint16_t count=0; bool eof=false,more=false;
                const auto status=stream_->read_body(wire_read_,pending_.data(),pending_.size(),count,eof,more);
                if(status==StatusCode::NotReady || status==StatusCode::DeviceBusy) break;
                if(status!=StatusCode::Ok) { failed(status); return; }
                if(count>pending_.size() || wire_read_>UINT32_MAX-count) { failed(StatusCode::IOError); return; }
                wire_read_+=count; pending_size_=count; wire_eof_=eof;
                if(!count) { if(eof) continue; break; }
                progress_=now_();
            }
            std::uint8_t byte=0; bool produced=false;
            if(!parser_.push(pending_[pending_at_++],byte,produced)) { failed(StatusCode::IOError); return; }
            if(produced) body_.push(&byte,1);
            if(parser_.headers_ready() && !headers_handled_ && !headers()) return;
            if(parser_.done()) { stream_->close(); return; }
        }
        if(body_.free() && now_()-progress_>=15000) failed(StatusCode::Timeout);
    }
    StatusCode write_body(std::uint32_t offset,const std::uint8_t* data,std::size_t length,std::uint16_t& written) override {
        written=0;
        if(length && !data) return StatusCode::InvalidRequest;
        if(commit_acknowledged_ && !length && offset==commit_offset_) return StatusCode::Ok;
        if(offset==last_write_offset_ && !last_write_.empty() && length>=last_write_.size() &&
           std::memcmp(data,last_write_.data(),last_write_.size())==0) { written=last_write_.size(); return StatusCode::Ok; }
        if(request_.method!=2 && request_.method!=3) return StatusCode::Unsupported;
        if(offset!=body_write_ || (!unknown_upload_ && length>request_.bodyLenHint-body_write_)) return StatusCode::InvalidRequest;
        if(error_!=StatusCode::Ok) return error_;
        if(!stream_) return StatusCode::NotReady;
        poll(); if(error_!=StatusCode::Ok) return error_;
        if(headers_sent_<request_headers_.size()) return StatusCode::NotReady;
        if(parser_.headers_ready()) return StatusCode::IOError; // server already sent its final response
        if(unknown_upload_) {
            if(upload_committed_) return length ? StatusCode::InvalidRequest : StatusCode::Ok;
            if(!upload_chunk_.empty()) return StatusCode::NotReady;
            if(!length) { upload_chunk_="0\r\n\r\n"; upload_committed_=true; commit_acknowledged_=true; commit_offset_=offset; return StatusCode::Ok; }
            const auto n=std::min<std::size_t>(length,4096);
            if(body_write_>UINT32_MAX-n) return StatusCode::InvalidRequest;
            char size[16]; std::snprintf(size,sizeof(size),"%zx\r\n",n);
            upload_chunk_=size; upload_chunk_.append(reinterpret_cast<const char*>(data),n); upload_chunk_+="\r\n";
            last_write_.assign(data,data+n); last_write_offset_=offset;
            body_write_+=n; written=n; return StatusCode::Ok;
        }
        const auto maximum=std::min<std::size_t>(length,4096);
        if(wire_write_>UINT32_MAX-maximum) return StatusCode::InvalidRequest;
        const auto status=stream_->write_body(wire_write_,data,maximum,written);
        if(status==StatusCode::Ok && written) {
            last_write_.assign(data,data+written); last_write_offset_=offset;
            body_write_+=written; wire_write_+=written; progress_=now_();
        }
        return status;
    }
    StatusCode read_body(std::uint32_t offset,std::uint8_t* out,std::size_t length,std::uint16_t& count,bool& eof,bool& more) override {
        count=0; eof=false; more=false;
        if(length && !out) return StatusCode::InvalidRequest;
        if(offset==last_read_offset_ && !last_read_.empty() && length>=last_read_.size()) {
            count=last_read_.size(); std::memcpy(out,last_read_.data(),count); eof=last_eof_; more=body_.size()!=0; return StatusCode::Ok;
        }
        if(offset!=body_read_) return StatusCode::InvalidRequest;
        poll();
        if(body_.size()) {
            count=body_.pop(out,std::min<std::size_t>(length,4096));
            eof=parser_.done() && !body_.size(); more=body_.size()!=0;
            last_read_.assign(out,out+count); last_read_offset_=offset; last_eof_=eof; body_read_+=count;
            return StatusCode::Ok;
        }
        if(error_!=StatusCode::Ok) return error_;
        eof=parser_.done(); return eof ? StatusCode::Ok : StatusCode::NotReady;
    }
    StatusCode info(io::NetworkInfo& out) override {
        poll(); out={}; if(error_!=StatusCode::Ok) return error_;
        if(!parser_.headers_ready()) return StatusCode::NotReady;
        out.hasHttpStatus=true; out.httpStatus=parser_.status();
        if(parser_.content_length()) { out.hasContentLength=true; out.contentLength=*parser_.content_length(); }
        for(const auto& [key,value]:parser_.headers()) {
            if(std::find(request_.responseHeaderNamesLower.begin(),request_.responseHeaderNamesLower.end(),key)!=request_.responseHeaderNamesLower.end()) out.headersBlock+=key+": "+value+"\r\n";
        }
        return StatusCode::Ok;
    }
    void close() override {
        if(stream_) stream_->close();
        stream_.reset(); request_={}; request_headers_.clear();
        parser_.reset(); body_.clear(); last_read_.clear(); last_write_.clear();
        pending_at_=pending_size_=headers_sent_=0; body_read_=body_write_=wire_read_=wire_write_=0;
        upload_chunk_.clear(); chunk_sent_=0; unknown_upload_=upload_committed_=false; commit_acknowledged_=false; commit_offset_=0;
        last_read_offset_=last_write_offset_=0; last_eof_=false; redirects_=0;
        error_=StatusCode::Ok; headers_handled_=wire_eof_=false;
    }
    bool read_ready() const override { return body_.size() || parser_.done() || error_!=StatusCode::Ok; }
    bool info_ready() const override { return parser_.headers_ready() || error_!=StatusCode::Ok; }
    bool read_available(std::size_t& count) const override { count=body_.size(); return true; }
    bool requires_sequential_read() const override { return true; }
    bool requires_sequential_write() const override { return true; }
};
}
std::unique_ptr<io::INetworkProtocol> make_http_protocol(HttpStreamFactory factory,std::function<std::uint64_t()> now) {
    return std::make_unique<HttpProtocol>(std::move(factory),std::move(now));
}
}
