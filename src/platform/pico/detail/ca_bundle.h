#pragma once
// Read-only view of the trusted-root bundle built by pico2w/tools/make_ca_bundle.py (the
// layout is documented there). Holds no state and allocates nothing: a lookup by subject name
// yields the DER of each matching root, and the caller parses only those.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fujinet::platform::pico::detail {

class CaBundle {
public:
    static constexpr std::size_t header_len = 16;
    static constexpr std::size_t entry_len = 16;

    CaBundle(const std::uint8_t* bytes, std::size_t size) : bytes_(bytes), size_(size) { validate(); }

    // False if the bundle is malformed. Every offset in it was checked once, so lookups on a
    // valid bundle cannot read outside it.
    bool valid() const { return valid_; }
    std::size_t count() const { return valid_ ? count_ : 0; }

    struct Root {
        const std::uint8_t* der;
        std::size_t der_len;
    };

    // The 32-bit FNV-1a hash the index is sorted by.
    static std::uint32_t hash_subject(const std::uint8_t* data, std::size_t length) {
        std::uint32_t value = 0x811C9DC5u;
        for (std::size_t i = 0; i < length; ++i) value = (value ^ data[i]) * 0x01000193u;
        return value;
    }

    // Calls fn(Root) for every root whose subject Name DER equals the given bytes. Returns how
    // many matched. Cross-signed CAs can share a subject, so more than one is possible.
    template <class Fn>
    std::size_t find_subject(const std::uint8_t* subject, std::size_t length, Fn&& fn) const {
        if (!valid_ || !subject || !length) return 0;
        const std::uint32_t wanted = hash_subject(subject, length);
        std::size_t low = 0, high = count_;
        while (low < high) { // first entry whose hash is not below the wanted one
            const std::size_t mid = low + (high - low) / 2;
            if (entry_hash(mid) < wanted) low = mid + 1; else high = mid;
        }
        std::size_t matched = 0;
        for (std::size_t i = low; i < count_ && entry_hash(i) == wanted; ++i) {
            const Entry entry = read_entry(i);
            const std::uint8_t* der = bytes_ + data_offset_ + entry.der_offset;
            if (entry.subject_len == length && std::memcmp(der + entry.subject_off, subject, length) == 0) {
                fn(Root{der, entry.der_len});
                ++matched;
            }
        }
        return matched;
    }

private:
    struct Entry {
        std::uint32_t hash, der_offset;
        std::uint16_t der_len, subject_off, subject_len;
    };

    static std::uint32_t u32(const std::uint8_t* p) {
        return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
               static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
    }
    static std::uint16_t u16(const std::uint8_t* p) {
        return static_cast<std::uint16_t>(p[0] | p[1] << 8);
    }
    Entry read_entry(std::size_t i) const {
        const std::uint8_t* p = bytes_ + header_len + i * entry_len;
        return Entry{u32(p), u32(p + 4), u16(p + 8), u16(p + 10), u16(p + 12)};
    }
    std::uint32_t entry_hash(std::size_t i) const { return u32(bytes_ + header_len + i * entry_len); }

    void validate() {
        valid_ = false;
        static constexpr char magic[8] = {'N', 'I', 'O', 'C', 'A', 'B', '0', '1'};
        if (!bytes_ || size_ < header_len || std::memcmp(bytes_, magic, sizeof(magic)) != 0) return;
        const std::uint32_t count = u32(bytes_ + 8);
        const std::uint32_t data_offset = u32(bytes_ + 12);
        if (count == 0 || count > (size_ - header_len) / entry_len) return;
        if (data_offset != header_len + static_cast<std::size_t>(count) * entry_len || data_offset > size_) return;
        const std::size_t data_size = size_ - data_offset;
        std::uint32_t previous = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const Entry e = read_entry(i);
            if (i && e.hash < previous) return; // the lookup binary-searches this order
            previous = e.hash;
            if (e.der_len == 0 || e.der_offset > data_size || e.der_len > data_size - e.der_offset) return;
            if (e.subject_len == 0 || e.subject_off > e.der_len || e.subject_len > e.der_len - e.subject_off) return;
            const std::uint8_t* subject = bytes_ + data_offset + e.der_offset + e.subject_off;
            if (hash_subject(subject, e.subject_len) != e.hash) return;
        }
        count_ = count;
        data_offset_ = data_offset;
        valid_ = true;
    }

    const std::uint8_t* bytes_;
    std::size_t size_;
    std::size_t count_ = 0;
    std::size_t data_offset_ = 0;
    bool valid_ = false;
};

} // namespace fujinet::platform::pico::detail
