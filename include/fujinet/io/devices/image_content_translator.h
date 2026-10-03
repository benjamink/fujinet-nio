#pragma once

#include "fujinet/core/large_stack.h"
#include "fujinet/io/devices/content_translator.h"
#include "fujinet/image/image_pipeline.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fujinet::io {

// Translation type 4 (Image): decodes PNG, JPEG or GIF and writes the indexed
// result in the format the selector asks for. See docs/network_device_protocol.md.
class ImageContentTranslator final : public IContentTranslator {
public:
    // Stack a conversion needs. stb_image keeps large tables on the stack;
    // -fstack-usage puts GIF at about 36 KB (stbi__gif_load alone is 35 KB),
    // PNG at about 8 KB and JPEG at about 10 KB, plus the pipeline above it.
    static constexpr std::size_t kConvertStackBytes = 48u * 1024u;

    // maxPixels: largest source image (width*height) to decode; larger ones
    // fail with Unsupported. The platform or fujinet.yaml supplies it
    // (translation.image.max_pixels), through NetworkDevice.
    // runner: runs each conversion on a kConvertStackBytes stack (the
    // platform's, through NetworkDevice); nullptr converts on the caller's
    // stack, which must then be that large.
    explicit ImageContentTranslator(std::uint32_t maxPixels,
                                    core::LargeStackRunner runner = nullptr);

    StatusCode configure(const TranslationConfig& config) override;
    void reset() override;
    StatusCode append_body(const std::uint8_t* data, std::size_t len) override;
    StatusCode finalize() override;
    StatusCode translate(const std::uint8_t* data, std::size_t len) override;
    std::uint64_t translated_size() const override;
    TranslationStats last_stats() const override;
    StatusCode read(std::uint32_t offset,
                    std::uint8_t* out,
                    std::size_t maxBytes,
                    std::uint16_t& actual,
                    bool& eof) const override;

    [[nodiscard]] std::uint32_t max_pixels() const noexcept { return _maxPixels; }

private:
    StatusCode convert(const std::uint8_t* data, std::size_t len);
    StatusCode convert_here(const std::uint8_t* data, std::size_t len);
    static void convert_job(void* job);

    image::Options _options{};
    std::vector<std::uint8_t> _body;
    std::vector<std::uint8_t> _out;
    std::uint32_t _maxPixels;
    core::LargeStackRunner _runner;
    TranslationStats _stats;
};

} // namespace fujinet::io
