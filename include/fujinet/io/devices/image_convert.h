// include/fujinet/io/devices/image_convert.h
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fujinet::io::image {

struct Options {
    int w = 640, h = 400, colors = 16, base = 0, parX = 1, parY = 1;
    bool dither = true;
    enum Mode { Auto, Gray, Color } mode = Auto;
    bool upscale = false;
};
struct Rgb { std::uint8_t r, g, b; };
struct Size { int w, h; };

bool parse_selector(const std::string& sel, Options& out);
int planes_for(const Options& o);
Size fit_size(int srcW, int srcH, const Options& o);
std::vector<std::uint8_t> scale_rgb(const std::uint8_t* rgb, int sw, int sh, Size out);
std::vector<Rgb> make_palette(const std::vector<std::uint8_t>& rgb, const Options& o);
std::vector<std::uint8_t> map_pixels(const std::vector<std::uint8_t>& rgb, Size s,
                                     const std::vector<Rgb>& pal, const Options& o);
std::vector<std::uint8_t> byterun1(const std::uint8_t* row, std::size_t n);
std::vector<std::uint8_t> write_ilbm(const std::vector<std::uint8_t>& idx, Size s,
                                     const std::vector<Rgb>& pal, const Options& o);
}
