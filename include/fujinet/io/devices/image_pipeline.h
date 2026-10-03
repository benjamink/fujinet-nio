// include/fujinet/io/devices/image_pipeline.h
#pragma once

#include "fujinet/io/core/io_message.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Format-neutral half of the Image translator: selector, decode, scale,
// palette, quantise and dither. It produces an IndexedImage; a writer (see
// image_writer_ilbm.h) turns that into the bytes a client reads.
namespace fujinet::io::image {

enum class ColourMode : std::uint8_t {
    Auto,
    Gray,
    Color,
};

struct Options {
    int w = 640;
    int h = 400;
    int colors = 16;
    int base = 0;
    int parX = 1;
    int parY = 1;
    bool dither = true;
    ColourMode mode = ColourMode::Auto;
    bool upscale = false;
};

struct Rgb {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

struct Size {
    int w;
    int h;
};

// Output of the pipeline. `pixels` holds one palette index (0..colors-1) per
// pixel, row by row. Writers add Options::base when they need pen numbers.
struct IndexedImage {
    Size size{0, 0};
    std::vector<std::uint8_t> pixels;
    std::vector<Rgb> palette;
};

// Parse the translator selector (`key=value,...`). Returns false for an
// unknown or repeated key, or a value out of range.
bool parse_selector(const std::string& selector, Options& out);

Size fit_size(int srcW, int srcH, const Options& o);
std::vector<std::uint8_t> scale_rgb(const std::uint8_t* rgb, int srcW, int srcH, Size out);
std::vector<Rgb> make_palette(const std::vector<std::uint8_t>& rgb, const Options& o);
std::vector<std::uint8_t> map_pixels(const std::vector<std::uint8_t>& rgb,
                                     Size size,
                                     const std::vector<Rgb>& palette,
                                     const Options& o);

// Decode a PNG, JPEG or GIF and run it through scale, palette and map.
//   InvalidRequest: the data is not an image stb_image can read.
//   Unsupported:    width*height exceeds maxPixels, or memory ran out.
StatusCode decode_to_indexed(const std::uint8_t* data,
                             std::size_t len,
                             std::uint32_t maxPixels,
                             const Options& o,
                             IndexedImage& out);

} // namespace fujinet::io::image
