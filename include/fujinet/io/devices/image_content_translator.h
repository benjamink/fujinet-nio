#pragma once
#include "fujinet/io/devices/content_translator.h"
#include "fujinet/io/devices/image_convert.h"
#include <vector>

namespace fujinet::io {

#ifndef FN_IMAGE_MAX_PIXELS
#  if defined(ESP_PLATFORM)
#    define FN_IMAGE_MAX_PIXELS (1200u * 1200u)   // RGB decode ~4.3 MB of PSRAM
#  else
#    define FN_IMAGE_MAX_PIXELS (4096u * 4096u)
#  endif
#endif

class ImageContentTranslator final : public IContentTranslator {
public:
    StatusCode configure(const TranslationConfig& config) override;
    void reset() override;
    StatusCode append_body(const std::uint8_t* data, std::size_t len) override;
    StatusCode finalize() override;
    std::uint64_t translated_size() const override { return _out.size(); }
    StatusCode read(std::uint32_t offset, std::uint8_t* out, std::size_t maxBytes,
                    std::uint16_t& actual, bool& eof) const override;
    void set_max_pixels_for_test(std::uint32_t n) { _maxPixels = n; }
private:
    image::Options _opt{};
    std::vector<std::uint8_t> _body, _out;
    std::uint32_t _maxPixels = FN_IMAGE_MAX_PIXELS;
};
}
