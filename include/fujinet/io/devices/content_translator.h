#pragma once

#include "fujinet/core/large_stack.h"
#include "fujinet/io/core/io_message.h"
#include "fujinet/io/devices/network_translation.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace fujinet::io {

// What a translator measured during its last translate() or finalize(), for
// diagnostics (net.translation.stats).
struct TranslationStats {
    core::LargeStackReport stack;   // all 0 when it ran on the caller's stack
    std::string detail;             // translator-specific, one line
};

class IContentTranslator {
public:
    virtual ~IContentTranslator() = default;

    virtual StatusCode configure(const TranslationConfig& config) = 0;
    virtual void reset() = 0;

    [[nodiscard]] virtual bool needs_full_body() const
    {
        return true;
    }

    virtual StatusCode append_body(const std::uint8_t* data, std::size_t len) = 0;
    virtual StatusCode finalize() = 0;

    // Translate a complete body. The caller keeps owning it (NetworkDevice
    // passes its response cache, which a later TranslateConfigure re-uses)
    // and it is only read during the call. The default copies it in through
    // append_body(); a translator that can work straight from the caller's
    // buffer overrides this so the body is not held twice.
    virtual StatusCode translate(const std::uint8_t* data, std::size_t len)
    {
        reset();
        const StatusCode appendSt = append_body(data, len);
        if (appendSt != StatusCode::Ok) {
            return appendSt;
        }
        return finalize();
    }

    [[nodiscard]] virtual std::uint64_t translated_size() const = 0;

    [[nodiscard]] virtual TranslationStats last_stats() const
    {
        return {};
    }

    virtual StatusCode read(std::uint32_t offset,
                            std::uint8_t* out,
                            std::size_t maxBytes,
                            std::uint16_t& actual,
                            bool& eof) const = 0;
};

} // namespace fujinet::io
