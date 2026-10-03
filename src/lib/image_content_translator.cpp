#include "fujinet/io/devices/image_content_translator.h"

#include "fujinet/io/devices/image_writer_ilbm.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace fujinet::io {

namespace {

// The one place a writer is chosen. parse_selector() only accepts formats
// listed here.
std::vector<std::uint8_t> write_output(const image::IndexedImage& indexed, const image::Options& options)
{
    switch (options.format) {
        case image::OutputFormat::Ilbm:
            return image::write_ilbm(indexed, options);
    }
    return {};
}

} // namespace

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

StatusCode ImageContentTranslator::convert(const std::uint8_t* data, std::size_t len)
{
    std::vector<std::uint8_t>().swap(_out);

    image::IndexedImage indexed;
    const StatusCode st = image::decode_to_indexed(data, len, _maxPixels, _options, indexed);
    if (st != StatusCode::Ok) {
        return st;
    }

    try {
        _out = write_output(indexed, _options);
    } catch (const std::bad_alloc&) {
        std::vector<std::uint8_t>().swap(_out);
        return StatusCode::Unsupported;
    }
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
