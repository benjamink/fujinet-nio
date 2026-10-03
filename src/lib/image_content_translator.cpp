#include "fujinet/io/devices/image_content_translator.h"
#include "stb_image.h"
#include <algorithm>
#include <cstring>

namespace fujinet::io {

StatusCode ImageContentTranslator::configure(const TranslationConfig& config) {
    if (config.type != ContentTranslationType::Image) return StatusCode::InvalidRequest;
    image::Options o;
    if (!image::parse_selector(config.selector, o)) return StatusCode::InvalidRequest;
    _opt = o; reset();
    return StatusCode::Ok;
}

void ImageContentTranslator::reset() { _body.clear(); _out.clear(); }

StatusCode ImageContentTranslator::append_body(const std::uint8_t* d, std::size_t n) {
    _body.insert(_body.end(), d, d + n);
    return StatusCode::Ok;
}

StatusCode ImageContentTranslator::finalize() {
    int w = 0, h = 0, comp = 0;
    if (_body.empty() || !stbi_info_from_memory(_body.data(), (int)_body.size(), &w, &h, &comp))
        return StatusCode::InvalidRequest;
    if ((std::uint64_t)w * (std::uint64_t)h > _maxPixels) { _body.clear(); return StatusCode::Unsupported; }
    std::uint8_t* px = stbi_load_from_memory(_body.data(), (int)_body.size(), &w, &h, &comp, 3);
    std::vector<std::uint8_t>().swap(_body);       // free the PNG before allocating the scaled copy
    if (!px) return StatusCode::InvalidRequest;
    image::Size s = image::fit_size(w, h, _opt);
    std::vector<std::uint8_t> rgb = image::scale_rgb(px, w, h, s);
    stbi_image_free(px);
    auto pal = image::make_palette(rgb, _opt);
    auto idx = image::map_pixels(rgb, s, pal, _opt);
    _out = image::write_ilbm(idx, s, pal, _opt);
    return StatusCode::Ok;
}

StatusCode ImageContentTranslator::read(std::uint32_t off, std::uint8_t* out, std::size_t max,
                                        std::uint16_t& actual, bool& eof) const {
    if (off > _out.size()) return StatusCode::InvalidRequest;
    std::size_t n = std::min<std::size_t>({max, _out.size() - off, 0xFFFFu});
    if (n) std::memcpy(out, _out.data() + off, n);
    actual = (std::uint16_t)n;
    eof = off + n >= _out.size();
    return StatusCode::Ok;
}
}
