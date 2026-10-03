#include "fujinet/io/devices/image_content_translator.h"

#include "fujinet/core/logging.h"
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

// The log macros compile away in release builds, taking every use of these.
[[maybe_unused]] const char* const TAG = "image";

[[maybe_unused]] const char* format_name(image::OutputFormat format)
{
    switch (format) {
        case image::OutputFormat::Ilbm:
            return "ilbm";
    }
    return "?";
}

[[maybe_unused]] unsigned whole_ms(std::uint32_t us)
{
    return static_cast<unsigned>(us / 1000);
}

[[maybe_unused]] unsigned frac_ms(std::uint32_t us)
{
    return static_cast<unsigned>(us % 1000);
}

} // namespace

// One line per conversion, so the cost on a target can be read from its log.
void ImageContentTranslator::log_timings([[maybe_unused]] const image::PipelineReport& report,
                                         [[maybe_unused]] image::Size output,
                                         [[maybe_unused]] std::uint32_t writeUs,
                                         [[maybe_unused]] std::uint32_t totalUs) const
{
    [[maybe_unused]] const image::PipelineTimings& t = report.timings;
    FN_LOGI(TAG,
            "%dx%d -> %dx%d %s bits=%d colors=%d: decode %u.%03u ms, scale %u.%03u ms, "
            "palette %u.%03u ms, quantise/dither %u.%03u ms, write %u.%03u ms, total %u.%03u ms, %u bytes",
            report.source.w, report.source.h,
            output.w, output.h,
            format_name(_options.format), _options.bits, _options.colors,
            whole_ms(t.decodeUs), frac_ms(t.decodeUs),
            whole_ms(t.scaleUs), frac_ms(t.scaleUs),
            whole_ms(t.paletteUs), frac_ms(t.paletteUs),
            whole_ms(t.mapUs), frac_ms(t.mapUs),
            whole_ms(writeUs), frac_ms(writeUs),
            whole_ms(totalUs), frac_ms(totalUs),
            static_cast<unsigned>(_out.size()));
}

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

    image::Stopwatch total;
    image::IndexedImage indexed;
    image::PipelineReport report;
    const StatusCode st = image::decode_to_indexed(data, len, _maxPixels, _options, indexed, report);
    if (st != StatusCode::Ok) {
        FN_LOGW(TAG, "%dx%d source not converted (status %u, cap %u pixels)",
                report.source.w,
                report.source.h,
                static_cast<unsigned>(st),
                static_cast<unsigned>(_maxPixels));
        return st;
    }

    image::Stopwatch write;
    try {
        _out = write_output(indexed, _options);
    } catch (const std::bad_alloc&) {
        std::vector<std::uint8_t>().swap(_out);
        return StatusCode::Unsupported;
    }
    const std::uint32_t writeUs = write.lap_us();
    log_timings(report, indexed.size, writeUs, total.lap_us());
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
