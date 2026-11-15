#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

namespace fujinet::platform::pico::detail {
// No SDK types: shared by raw callbacks and host tests. Caller owns locking.
template<std::size_t Capacity> class ByteRing {
    std::array<std::uint8_t, Capacity> bytes_{};
    std::size_t head_ = 0, size_ = 0;
public:
    std::size_t size() const { return size_; }
    std::size_t free() const { return Capacity - size_; }
    bool push(const void* source, std::size_t n) {
        if (n > free()) return false;
        auto* p = static_cast<const std::uint8_t*>(source);
        for (std::size_t i = 0; i < n; ++i) bytes_[(head_ + size_ + i) % Capacity] = p[i];
        size_ += n;
        return true;
    }
    std::size_t pop(void* dest, std::size_t n) {
        n = std::min(n, size_);
        auto* p = static_cast<std::uint8_t*>(dest);
        for (std::size_t i = 0; i < n; ++i) p[i] = bytes_[(head_ + i) % Capacity];
        head_ = (head_ + n) % Capacity;
        size_ -= n;
        return n;
    }
};

template<class T, std::size_t Capacity> class HandleTable {
    struct Slot { int handle = -1; T value{}; };
    std::array<Slot, Capacity> slots_{};
    int next_ = 1;
public:
    int allocate() {
        // Never reuse a handle, even after close. Exhaustion fails safely.
        if (next_ == std::numeric_limits<int>::max()) return -1;
        for (auto& s : slots_) if (s.handle < 0) {
            std::destroy_at(&s.value); std::construct_at(&s.value);
            s.handle = next_++;
            return s.handle;
        }
        return -1;
    }
    T* get(int handle) {
        if (handle < 0) return nullptr;
        for (auto& s : slots_) if (s.handle == handle) return &s.value;
        return nullptr;
    }
    void release(int handle) {
        for (auto& s : slots_) if (s.handle == handle) { s.handle = -1; std::destroy_at(&s.value); std::construct_at(&s.value); return; }
    }
};

template<std::size_t MaxPacket, std::size_t Depth> class DatagramQueue {
    struct Packet { std::array<std::uint8_t, MaxPacket> data{}; std::size_t size = 0; };
    std::array<Packet, Depth> packets_{};
    std::size_t head_ = 0, count_ = 0;
public:
    bool empty() const { return count_ == 0; }
    bool full() const { return count_ == Depth; }
    template<class Copy> bool emplace(std::size_t n, Copy copy) {
        if (full() || n > MaxPacket) return false;
        auto& packet = packets_[(head_ + count_) % Depth];
        copy(packet.data.data(), n);
        packet.size = n;
        ++count_;
        return true;
    }
    bool push(const void* p, std::size_t n) {
        return emplace(n, [p](void* dest, std::size_t size) { if (size) std::memcpy(dest,p,size); });
    }
    std::size_t pop(void* out, std::size_t max) {
        if (empty()) return 0;
        auto& packet = packets_[head_];
        const auto n = std::min(max, packet.size);
        if (n) std::memcpy(out, packet.data.data(), n);
        head_ = (head_ + 1) % Depth;
        --count_;
        return n; // Always discard the rest of this packet.
    }
};
}
