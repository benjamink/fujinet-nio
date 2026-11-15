#pragma once
#include "http_request.h"
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace fujinet::platform::pico::detail {
// Incremental HTTP/1.x response decoder. Caller supplies a bounded body queue
// and only calls push when it can accept one byte. Metadata is capped at 8KiB.
class HttpParser {
    enum class State { Status, Headers, Length, UntilClose, ChunkSize, ChunkData, ChunkCR, ChunkLF, Trailers, Done, Error };
    State state_=State::Status;
    std::string line_;
    bool cr_=false, head_=false, ready_=false, chunked_=false;
    unsigned interim_=0, status_=0;
    std::size_t metadata_bytes_=0;
    std::uint64_t remaining_=0, body_count_=0;
    std::optional<std::uint64_t> length_;
    std::vector<std::pair<std::string,std::string>> headers_;
    static std::string trim(std::string text) {
        const auto a=text.find_first_not_of(" \t"); if(a==std::string::npos) return {};
        return text.substr(a,text.find_last_not_of(" \t")-a+1);
    }
    bool fail() { state_=State::Error; return false; }
    bool decimal(const std::string& value,std::uint64_t& result) {
        if(value.empty()) return false;
        result=0;
        for(char c:value) {
            if(c<'0'||c>'9' || result>(UINT32_MAX-(c-'0'))/10) return false;
            result=result*10+c-'0';
        }
        return true;
    }
    bool completed_line() {
        const auto line=std::move(line_); line_.clear();
        if(state_==State::Status) {
            if(line.size()<12 || (line.compare(0,9,"HTTP/1.1 ")!=0 && line.compare(0,9,"HTTP/1.0 ")!=0) ||
               (line.size()>12 && line[12]!=' ')) return fail();
            status_=0;
            for(unsigned i=9;i<12;++i) { if(line[i]<'0'||line[i]>'9') return fail(); status_=status_*10+line[i]-'0'; }
            if(status_<100 || status_>599 || status_==101) return fail();
            state_=State::Headers; return true;
        }
        if(state_==State::ChunkSize) {
            const auto end=line.find(';'); const auto number=line.substr(0,end);
            if(number.empty() || number.size()>8) return fail();
            remaining_=0;
            for(char c:number) {
                unsigned digit;
                if(c>='0'&&c<='9') digit=c-'0'; else if(c>='a'&&c<='f') digit=c-'a'+10;
                else if(c>='A'&&c<='F') digit=c-'A'+10; else return fail();
                remaining_=remaining_*16+digit;
            }
            if(remaining_>UINT32_MAX-body_count_) return fail();
            state_=remaining_ ? State::ChunkData : State::Trailers; return true;
        }
        if(line.empty()) {
            if(state_==State::Trailers) { state_=State::Done; return true; }
            if(status_<200) {
                if(++interim_>4) return fail();
                state_=State::Status; headers_.clear(); length_.reset(); chunked_=false; return true;
            }
            if(chunked_ && length_) return fail();
            ready_=true;
            if(head_ || status_==204 || status_==304) state_=State::Done;
            else if(chunked_) state_=State::ChunkSize;
            else if(length_) { remaining_=*length_; state_=remaining_ ? State::Length : State::Done; }
            else state_=State::UntilClose;
            return true;
        }
        const auto colon=line.find(':');
        if(colon==std::string::npos || !http_request_token(std::string_view(line).substr(0,colon))) return fail();
        const auto name=http_request_lower(line.substr(0,colon)); const auto value=trim(line.substr(colon+1));
        if(!http_request_value(value)) return fail();
        if(state_==State::Trailers) {
            if(name=="content-length" || name=="transfer-encoding" || name=="host") return fail();
            return true; // Trailer metadata is not exposed as initial response headers.
        }
        if(headers_.size()>=64) return fail();
        headers_.emplace_back(name,value);
        if(name=="content-length") {
            std::uint64_t value_number;
            if(!decimal(value,value_number) || (length_ && *length_!=value_number)) return fail();
            length_=value_number;
        }
        if(name=="transfer-encoding") {
            if(chunked_ || http_request_lower(value)!="chunked") return fail();
            chunked_=true;
        }
        return true;
    }
public:
    explicit HttpParser(bool head=false):head_(head) {}
    void reset(bool head=false) { *this=HttpParser(head); }
    bool push(std::uint8_t byte,std::uint8_t& body,bool& produced) {
        produced=false;
        if(state_==State::Error || state_==State::Done) return false;
        if(state_==State::Length || state_==State::UntilClose || state_==State::ChunkData) {
            if(body_count_==UINT32_MAX) return fail();
            body=byte; produced=true; ++body_count_;
            if(state_==State::Length && --remaining_==0) state_=State::Done;
            else if(state_==State::ChunkData && --remaining_==0) state_=State::ChunkCR;
            return true;
        }
        if(state_==State::ChunkCR) { if(byte!='\r') return fail(); state_=State::ChunkLF; return true; }
        if(state_==State::ChunkLF) { if(byte!='\n') return fail(); state_=State::ChunkSize; return true; }
        if(state_!=State::ChunkSize && ++metadata_bytes_>8192) return fail();
        if(cr_) { cr_=false; if(byte!='\n') return fail(); return completed_line(); }
        if(byte=='\r') { cr_=true; return true; }
        if(byte=='\n' || byte==0 || byte==127 || (byte<32 && byte!='\t')) return fail();
        const std::size_t limit=state_==State::ChunkSize ? 128 : 2048;
        if(line_.size()>=limit) return fail();
        line_.push_back(byte); return true;
    }
    bool finish() { if(state_==State::UntilClose) state_=State::Done; return state_==State::Done ? true : fail(); }
    bool headers_ready() const { return ready_; }
    bool done() const { return state_==State::Done; }
    bool failed() const { return state_==State::Error; }
    unsigned status() const { return status_; }
    const std::optional<std::uint64_t>& content_length() const { return length_; }
    const auto& headers() const { return headers_; }
};
}
