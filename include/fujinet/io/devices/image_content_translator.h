#pragma once

#include "fujinet/io/devices/content_translator.h"
#include "fujinet/io/devices/image_pipeline.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fujinet::io {

// Translation type 4 (Image): decodes PNG, JPEG or GIF and writes the indexed
// result in the format the selector asks for. See docs/network_device_protocol.md.
class ImageContentTranslator final : public IContentTranslator {
public:
    // maxPixels: largest source image (width*height) to decode; larger ones
    // fail with Unsupported. The platform or fujinet.yaml supplies it
    // (network.image_max_pixels), through NetworkDevice.
    explicit ImageContentTranslator(std::uint32_t maxPixels);

    StatusCode configure(const TranslationConfig& config) override;
    void reset() override;
    StatusCode append_body(const std::uint8_t* data, std::size_t len) override;
    StatusCode finalize() override;
    StatusCode translate(const std::uint8_t* data, std::size_t len) override;
    std::uint64_t translated_size() const override;
    StatusCode read(std::uint32_t offset,
                    std::uint8_t* out,
                    std::size_t maxBytes,
                    std::uint16_t& actual,
                    bool& eof) const override;

    [[nodiscard]] std::uint32_t max_pixels() const noexcept { return _maxPixels; }

private:
    StatusCode convert(const std::uint8_t* data, std::size_t len);

    image::Options _options{};
    std::vector<std::uint8_t> _body;
    std::vector<std::uint8_t> _out;
    std::uint32_t _maxPixels;
};

} // namespace fujinet::io
