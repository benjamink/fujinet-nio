#pragma once
#include "socket_state.h"

namespace fujinet::platform::pico::detail {
// Fixed storage outlives every Channel. Lock is a Pico SDK mutex in firmware and
// std::mutex in native tests. Nothing invokes application code while locked.
template<class Lock, std::size_t RxCapacity=4096, std::size_t TxCapacity=16384>
class UsbBridge {
    struct Guard {
        Lock& lock;
        explicit Guard(Lock& l): lock(l) { lock.lock(); }
        ~Guard() { lock.unlock(); }
    };
    mutable Lock lock_;
    ByteRing<RxCapacity> rx_;
    ByteRing<TxCapacity> tx_;
    std::array<std::uint8_t,64> staged_{};
    std::size_t staged_size_=0, staged_offset_=0;
    bool attached_=false, connected_=false, fault_=false;
    std::uint32_t epoch_=0, serviced_epoch_=0;
    bool ready() const { return attached_ && connected_ && !fault_; }
    void invalidate() {
        rx_.clear(); tx_.clear(); staged_size_=staged_offset_=0; ++epoch_;
    }
    bool enqueue(const void* p,std::size_t n) {
        if(!ready()) return false;
        // Staged, partly accepted bytes count toward the same bounded capacity.
        if(n>tx_.free()-(staged_size_-staged_offset_)) {
            fault_=true; invalidate(); return false;
        }
        return tx_.push(p,n);
    }
public:
    bool attach() {
        Guard guard(lock_);
        if(attached_) return false;
        attached_=true; invalidate(); return true;
    }
    void detach() {
        Guard guard(lock_); attached_=false; invalidate();
    }
    void connection(bool connected) {
        Guard guard(lock_);
        if(connected_!=connected) {
            connected_=connected; fault_=false; invalidate();
        }
    }
    // Reset is an edge even when DTR was already false.
    void reset() {
        Guard guard(lock_); connected_=false; fault_=false; invalidate();
    }
    // TinyUSB configuration_reset calls the observer before resetting CDC.
    // No CDC calls here: the class reset clears its FIFOs later in that same
    // task event, before any new-session traffic can be dispatched.
    void reset_from_device() {
        Guard guard(lock_); connected_=false; fault_=false; invalidate();
        serviced_epoch_=epoch_;
    }
    bool available() const { Guard guard(lock_); return ready() && rx_.size(); }
    std::size_t read(void* p,std::size_t n,std::uint32_t* observed_epoch=nullptr) {
        Guard guard(lock_);
        const auto count=ready() ? rx_.pop(p,n) : 0;
        if(count && observed_epoch) *observed_epoch=epoch_;
        return count;
    }
    bool write(const void* p,std::size_t n) {
        Guard guard(lock_); return enqueue(p,n);
    }
    bool write_if_epoch(const void* p,std::size_t n,std::uint32_t observed_epoch) {
        Guard guard(lock_);
        return observed_epoch==epoch_ && enqueue(p,n);
    }
    std::uint32_t epoch() const { Guard guard(lock_); return epoch_; }

    // Task-context session callbacks clear CDC immediately, before tud_task can
    // dispatch another completion. Acknowledging the epoch prevents the later
    // pump from discarding data received in the new session during that task.
    template<class Device> void clear_device(Device& device) {
        Guard guard(lock_);
        device.clear_rx(); device.clear_tx(); serviced_epoch_=epoch_;
    }

    // Only core 1 calls this. Device methods must be bounded FIFO operations,
    // never tud_task, waits, logging, allocation or callbacks into the bridge.
    // Holding the lock across submit serializes it with detach and quarantine.
    template<class Device> void pump(Device& device) {
        Guard guard(lock_);
        if(serviced_epoch_!=epoch_ || !ready()) {
            device.clear_rx(); device.clear_tx(); serviced_epoch_=epoch_;
        }
        if(!ready()) return;
        std::uint8_t input[64];
        const auto room=std::min(rx_.free(),sizeof(input));
        if(room) {
            const auto count=device.read(input,room);
            rx_.push(input,count);
        }
        // Full RX leaves bytes in CDC, allowing USB endpoint backpressure.
        if(staged_size_==staged_offset_) {
            staged_size_=tx_.pop(staged_.data(),staged_.size()); staged_offset_=0;
        }
        const auto count=std::min(staged_size_-staged_offset_,device.write_available());
        if(count) staged_offset_+=device.write(staged_.data()+staged_offset_,count);
        device.flush();
    }
};
}
