#include "fujinet/io/devices/image_content_translator.h"

#include "fujinet/core/logging.h"
#include "fujinet/image/output_format.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace fujinet::io {

namespace {

// The log macros compile away in release builds, taking every use of this.
[[maybe_unused]] const char* const TAG = "image";

} // namespace

ImageContentTranslator::ImageContentTranslator(std::uint32_t maxPixels)
    : _maxPixels(maxPixels)
{}

StatusCode ImageContentTranslator::configure(const TranslationConfig& config)
{
    if (config.type != ContentTranslationType::Image) {
        return StatusCode::InvalidRequest;
    }

    image::Options options;
    if (!image::parse_selector(config.selector, options)) {
        return StatusCode::InvalidRequest;
    }

    _options = options;
    reset();
    return StatusCode::Ok;
}

void ImageContentTranslator::reset()
{
    std::vector<std::uint8_t>().swap(_body);
    std::vector<std::uint8_t>().swap(_out);
}

StatusCode ImageContentTranslator::append_body(const std::uint8_t* data, std::size_t len)
{
    if (len == 0) {
        return StatusCode::Ok;
    }
    if (data == nullptr) {
        return StatusCode::InvalidRequest;
    }
    _body.insert(_body.end(), data, data + len);
    return StatusCode::Ok;
}

StatusCode ImageContentTranslator::finalize()
{
    std::vector<std::uint8_t> body;
    body.swap(_body);
    return convert(body.data(), body.size());
}

// Decodes straight from the caller's buffer: no copy of the body is made.
StatusCode ImageContentTranslator::translate(const std::uint8_t* data, std::size_t len)
{
    std::vector<std::uint8_t>().swap(_body);
    return convert(data, len);
}

StatusCode ImageContentTranslator::convert(const std::uint8_t* data, std::size_t len)
{
    std::vector<std::uint8_t>().swap(_out);
    if (_options.format == nullptr) {
        return StatusCode::InvalidRequest;      // not configured
    }

    image::IndexedImage indexed;
    image::Size source{0, 0};
    const StatusCode st = image::decode_to_indexed(data, len, _maxPixels, _options, indexed, source);
    if (st != StatusCode::Ok) {
        FN_LOGW(TAG, "%dx%d source not converted (status %u, cap %u pixels)",
                source.w,
                source.h,
                static_cast<unsigned>(st),
                static_cast<unsigned>(_maxPixels));
        return st;
    }

    try {
        _out = _options.format->write(indexed, _options);
    } catch (const std::bad_alloc&) {
        std::vector<std::uint8_t>().swap(_out);
        return StatusCode::Unsupported;
    }
    FN_LOGI(TAG, "%dx%d -> %dx%d %.*s bits=%d colors=%d, %u bytes",
            source.w, source.h,
            indexed.size.w, indexed.size.h,
            static_cast<int>(_options.format->name.size()), _options.format->name.data(),
            _options.bits, _options.colors,
            static_cast<unsigned>(_out.size()));
    return StatusCode::Ok;
}

std::uint64_t ImageContentTranslator::translated_size() const
{
    return static_cast<std::uint64_t>(_out.size());
}

StatusCode ImageContentTranslator::read(std::uint32_t offset,
                                        std::uint8_t* out,
                                        std::size_t maxBytes,
                                        std::uint16_t& actual,
                                        bool& eof) const
{
    if (offset > _out.size()) {
        return StatusCode::InvalidRequest;
    }

    const std::size_t remaining = _out.size() - offset;
    const std::size_t n = std::min<std::size_t>({maxBytes, remaining, 0xFFFFu});
    if (n > 0) {
        std::memcpy(out, _out.data() + offset, n);
    }
    actual = static_cast<std::uint16_t>(n);
    eof = offset + n >= _out.size();
    return StatusCode::Ok;
}

} // namespace fujinet::io
