#pragma once

#include "fujinet/io/devices/content_translator.h"
#include "fujinet/io/devices/image_pipeline.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fujinet::io {

#ifndef FN_IMAGE_MAX_PIXELS
#  if defined(ESP_PLATFORM)
// stb's PNG path holds the compressed IDAT copy, the inflate buffer and the
    // decoded output at once, roughly 3-4 bytes/px at peak, on top of the cached
    // response body and _body. 700x700 (0.49 Mpx) keeps that near 2 MB.
    // Not yet verified on hardware.
#    define FN_IMAGE_MAX_PIXELS (700u * 700u)
#  else
#    define FN_IMAGE_MAX_PIXELS (4096u * 4096u)
#  endif
#endif

// Translation type 4 (Image): decodes PNG, JPEG or GIF and writes the indexed
// result in the format the selector asks for. See docs/network_device_protocol.md.
class ImageContentTranslator final : public IContentTranslator {
public:
    StatusCode configure(const TranslationConfig& config) override;
    void reset() override;
    StatusCode append_body(const std::uint8_t* data, std::size_t len) override;
    StatusCode finalize() override;
    std::uint64_t translated_size() const override;
    StatusCode read(std::uint32_t offset,
                    std::uint8_t* out,
                    std::size_t maxBytes,
                    std::uint16_t& actual,
                    bool& eof) const override;

    void set_max_pixels_for_test(std::uint32_t n) { _maxPixels = n; }

private:
    StatusCode convert(const std::uint8_t* data, std::size_t len);

    image::Options _options{};
    std::vector<std::uint8_t> _body;
    std::vector<std::uint8_t> _out;
    std::uint32_t _maxPixels = FN_IMAGE_MAX_PIXELS;
};

} // namespace fujinet::io
