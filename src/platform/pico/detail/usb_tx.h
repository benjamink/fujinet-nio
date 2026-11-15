#pragma once
#include "socket_state.h"
namespace fujinet::platform::pico::detail {
// Channel::write cannot return a short write. Never emit a truncated response
// followed by another response: quarantine overflow until a physical reconnect.
template<std::size_t Capacity> class UsbTx {
    ByteRing<Capacity> bytes_;
    bool connected_ = false, fault_ = false;
    std::uint32_t epoch_ = 0;
public:
    void connection(bool connected) {
        if(connected_ != connected) { connected_=connected; fault_=false; bytes_.clear(); ++epoch_; }
    }
    bool ready() const { return connected_ && !fault_; }
    std::uint32_t epoch() const { return epoch_; }
    bool write(const void* p,std::size_t n) {
        if(!ready()) return false;
        if(!bytes_.push(p,n)) { fault_=true; ++epoch_; return false; }
        return true;
    }
    std::size_t take(void* p,std::size_t n) { return ready() ? bytes_.pop(p,n) : 0; }
};
}
