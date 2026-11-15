#pragma once
#include "fujinet/io/devices/network_protocol.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
namespace fujinet::platform::pico::detail {
struct HttpUrl { bool tls=false; std::string host, authority, target; std::uint16_t port=80; };
inline std::string http_request_lower(std::string text) { for(auto& c:text) if(c>='A' && c<='Z') c+=32; return text; }
inline bool http_request_token(std::string_view text) {
    if(text.empty()) return false;
    constexpr std::string_view symbols="!#$%&'*+-.^_`|~";
    for(unsigned char c:text) if(!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || symbols.find(c)!=std::string_view::npos)) return false;
    return true;
}
inline bool http_request_value(std::string_view text) {
    for(unsigned char c:text) if((c<32 && c!='\t') || c==127) return false;
    return true;
}
inline bool parse_http_url(const std::string& url,HttpUrl& out) {
    out={}; if(url.size()>2048) return false;
    std::size_t start;
    if(url.rfind("http://",0)==0) start=7;
    else if(url.rfind("https://",0)==0) { start=8; out.tls=true; out.port=443; }
    else return false;
    const auto end=url.find_first_of("/?#",start);
    out.authority=url.substr(start,end==std::string::npos ? end : end-start);
    if(out.authority.empty()) return false;
    const auto colon=out.authority.find(':');
    out.host=out.authority.substr(0,colon);
    if(out.host.empty() || out.host.size()>253) return false;
    for(unsigned char c:out.host) if(!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='.' || c=='-')) return false;
    if(colon!=std::string::npos) {
        unsigned port=0; const auto text=out.authority.substr(colon+1);
        if(text.empty() || text.size()>5) return false;
        for(char c:text) { if(c<'0'||c>'9') return false; port=port*10+c-'0'; }
        if(!port || port>65535) return false;
        out.port=port;
    }
    out.target=end==std::string::npos ? "/" : url.substr(end);
    out.target.resize(out.target.find('#')==std::string::npos ? out.target.size() : out.target.find('#'));
    if(out.target.empty() || out.target.front()!='/' ) out.target="/"+out.target;
    for(unsigned char c:out.target) if(c<=32 || c==127) return false;
    return true;
}
inline bool build_http_request(const io::NetworkOpenRequest& req,HttpUrl& url,std::string& headers) {
    if(!parse_http_url(req.url,url) || req.method<1 || req.method>5 || req.headers.size()>64) return false;
    const bool unknown=(req.flags&4) && !req.bodyLenHint && (req.method==2 || req.method==3);
    constexpr const char* methods[]={"","GET","POST","PUT","DELETE","HEAD"};
    headers=std::string(methods[req.method])+" "+url.target+" HTTP/1.1\r\nHost: "+url.authority+"\r\nConnection: close\r\n";
    std::size_t input_bytes=0;
    for(const auto& [key,value]:req.headers) {
        if(key.size()>256 || value.size()>8192 || key.size()+value.size()>8192-input_bytes) return false;
        input_bytes+=key.size()+value.size();
        if(!http_request_token(key) || !http_request_value(value)) return false;
        const auto name=http_request_lower(key);
        if(name=="host" || name=="connection") continue;
        if(name=="transfer-encoding" || name=="expect") return false;
        if(name=="content-length") {
            if(unknown || value!=std::to_string(req.bodyLenHint)) return false;
            continue;
        }
        if(key.size()+value.size()>8192-std::min<std::size_t>(headers.size(),8192)) return false;
        headers+=key+": "+value+"\r\n";
    }
    if(unknown) headers+="Transfer-Encoding: chunked\r\n";
    else if(req.bodyLenHint || req.method==2 || req.method==3) headers+="Content-Length: "+std::to_string(req.bodyLenHint)+"\r\n";
    headers+="\r\n"; return headers.size()<=8192;
}
inline std::string remove_dot_segments(std::string input) {
    std::string output;
    auto pop=[&] { const auto slash=output.rfind('/'); output.resize(slash==std::string::npos ? 0 : slash); };
    while(!input.empty()) {
        if(input.rfind("../",0)==0) input.erase(0,3);
        else if(input.rfind("./",0)==0) input.erase(0,2);
        else if(input.rfind("/./",0)==0) input.erase(0,2);
        else if(input=="/.") input="/";
        else if(input.rfind("/../",0)==0) { input.erase(0,3); pop(); }
        else if(input=="/..") { input="/"; pop(); }
        else if(input=="." || input=="..") input.clear();
        else {
            const auto end=input.find('/',input.front()=='/' ? 1 : 0);
            if(end==std::string::npos) { output+=input; input.clear(); }
            else { output+=input.substr(0,end); input.erase(0,end); }
        }
    }
    return output.empty() ? "/" : output;
}
inline std::string redirect_url(const HttpUrl& base,const std::string& location) {
    const std::string scheme=base.tls ? "https:" : "http:";
    const auto origin=scheme+"//"+base.authority;
    std::string resolved;
    if(location.rfind("http://",0)==0 || location.rfind("https://",0)==0) resolved=location;
    else if(location.rfind("//",0)==0) resolved=scheme+location;
    else if(location.empty() || location.front()=='#') resolved=origin+base.target+location;
    else if(location.front()=='/') resolved=origin+location;
    else if(location.front()=='?') resolved=origin+base.target.substr(0,base.target.find('?'))+location;
    else {
        // A colon in the first segment denotes an unsupported URI scheme.
        const auto colon=location.find(':'); const auto slash=location.find('/');
        if(colon!=std::string::npos && (slash==std::string::npos || colon<slash)) return {};
        const auto path=base.target.substr(0,base.target.find('?'));
        resolved=origin+path.substr(0,path.rfind('/')+1)+location;
    }
    HttpUrl parsed;
    if(!parse_http_url(resolved,parsed)) return {};
    const auto query=parsed.target.find('?');
    const auto normalized=remove_dot_segments(parsed.target.substr(0,query));
    return std::string(parsed.tls ? "https://" : "http://")+parsed.authority+normalized+
           (query==std::string::npos ? "" : parsed.target.substr(query));
}
}
