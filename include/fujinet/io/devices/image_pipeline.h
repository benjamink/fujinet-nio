// include/fujinet/io/devices/image_pipeline.h
#pragma once

#include "fujinet/io/core/io_message.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Format-neutral half of the Image translator: selector, decode, scale,
// palette, quantise and dither. It produces an IndexedImage; a writer (see
// image_writer_ilbm.h) turns that into the bytes a client reads.
namespace fujinet::io::image {

enum class OutputFormat : std::uint8_t {
    Ilbm,
};

// Palette depth (bits per RGB channel) used when the selector has no `bits`
// key. The one place each format's default lives.
constexpr int default_palette_bits(OutputFormat format)
{
    switch (format) {
        case OutputFormat::Ilbm:
            return 4;       // Amiga OCS: 12-bit colour
    }
    return 4;
}

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
    int bits = default_palette_bits(OutputFormat::Ilbm);   // bits per RGB channel
    bool dither = true;
    ColourMode mode = ColourMode::Auto;
    bool upscale = false;
    OutputFormat format = OutputFormat::Ilbm;
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

// Time spent in each stage of decode_to_indexed(), in microseconds.
struct PipelineTimings {
    std::uint32_t decodeUs = 0;
    std::uint32_t scaleUs = 0;
    std::uint32_t paletteUs = 0;
    std::uint32_t mapUs = 0;            // quantise and dither
};

// What decode_to_indexed() learnt, for logging; filled in as far as it got.
struct PipelineReport {
    Size source{0, 0};
    PipelineTimings timings;
};

// Times the stages of a conversion with the monotonic clock.
class Stopwatch {
public:
    Stopwatch();
    // Microseconds since construction or the previous lap.
    std::uint32_t lap_us();

private:
    std::chrono::steady_clock::time_point _last;
};

// Highest value accepted for the `bits` selector key (full 24-bit colour).
constexpr int kMaxPaletteBits = 8;

// Parse the translator selector (`key=value,...`). Returns false for an
// unknown or repeated key, a value out of range, or an unknown `fmt`.
bool parse_selector(const std::string& selector, Options& out);

// Snap an 8-bit channel value to the nearest of 2^bits evenly spaced levels
// (0 and 255 included). bits=4 is the Amiga OCS grid, bits=8 leaves it alone.
std::uint8_t snap_channel(int value, int bits);

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
                             IndexedImage& out,
                             PipelineReport& report);

} // namespace fujinet::io::image
