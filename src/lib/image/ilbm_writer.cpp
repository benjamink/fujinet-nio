// src/lib/image/ilbm_writer.cpp
#include "fujinet/image/ilbm_writer.h"

#include <algorithm>

namespace fujinet::image::ilbm {

namespace {

constexpr std::size_t kMaxRun = 128;
constexpr std::uint8_t kCompressionByteRun1 = 1;

void put_u32_be(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void put_u16_be(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void put_tag(std::vector<std::uint8_t>& out, const char* tag)
{
    out.insert(out.end(), tag, tag + 4);
}

void patch_u32_be(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value)
{
    out[at] = static_cast<std::uint8_t>(value >> 24);
    out[at + 1] = static_cast<std::uint8_t>(value >> 16);
    out[at + 2] = static_cast<std::uint8_t>(value >> 8);
    out[at + 3] = static_cast<std::uint8_t>(value);
}

std::size_t repeat_length(const std::uint8_t* row, std::size_t n, std::size_t i)
{
    std::size_t run = 1;
    while (i + run < n && run < kMaxRun && row[i + run] == row[i]) {
        ++run;
    }
    return run;
}

// Literal bytes up to (not including) the next pair of equal bytes.
std::size_t literal_length(const std::uint8_t* row, std::size_t n, std::size_t i)
{
    std::size_t len = 1;
    while (i + len < n && len < kMaxRun) {
        const bool pairStartsHere = i + len + 1 < n && row[i + len] == row[i + len + 1];
        if (pairStartsHere) {
            break;
        }
        ++len;
    }
    return len;
}

void write_bmhd(std::vector<std::uint8_t>& out, Size size, int planes, const Options& o)
{
    put_tag(out, "BMHD");
    put_u32_be(out, 20);
    put_u16_be(out, static_cast<std::uint16_t>(size.w));
    put_u16_be(out, static_cast<std::uint16_t>(size.h));
    put_u16_be(out, 0);                                     // x
    put_u16_be(out, 0);                                     // y
    out.push_back(static_cast<std::uint8_t>(planes));
    out.push_back(0);                                       // masking: none
    out.push_back(kCompressionByteRun1);
    out.push_back(0);                                       // pad
    put_u16_be(out, 0);                                     // transparentColor
    out.push_back(static_cast<std::uint8_t>(o.parX));       // xAspect
    out.push_back(static_cast<std::uint8_t>(o.parY));       // yAspect
    put_u16_be(out, static_cast<std::uint16_t>(size.w));    // pageWidth
    put_u16_be(out, static_cast<std::uint16_t>(size.h));    // pageHeight
}

// One entry per pen; pens outside base..base+colors-1 are black.
void write_cmap(std::vector<std::uint8_t>& out, const std::vector<Rgb>& palette, int planes, const Options& o)
{
    const int pens = 1 << planes;
    put_tag(out, "CMAP");
    put_u32_be(out, static_cast<std::uint32_t>(3 * pens));
    for (int pen = 0; pen < pens; ++pen) {
        const int index = pen - o.base;
        if (index >= 0 && index < static_cast<int>(palette.size())) {
            const Rgb& c = palette[static_cast<std::size_t>(index)];
            out.push_back(c.r);
            out.push_back(c.g);
            out.push_back(c.b);
        } else {
            out.push_back(0);
            out.push_back(0);
            out.push_back(0);
        }
    }
}

// Set bit `plane` of each pixel's pen into a bitplane row.
void fill_plane_row(std::vector<std::uint8_t>& row,
                    const std::uint8_t* pixels,
                    int width,
                    int plane,
                    int base)
{
    std::fill(row.begin(), row.end(), 0);
    for (int x = 0; x < width; ++x) {
        const int pen = base + pixels[x];
        if ((pen & (1 << plane)) != 0) {
            row[static_cast<std::size_t>(x >> 3)] |= static_cast<std::uint8_t>(0x80 >> (x & 7));
        }
    }
}

void write_body(std::vector<std::uint8_t>& out, const IndexedImage& image, int planes, const Options& o)
{
    put_tag(out, "BODY");
    const std::size_t lengthAt = out.size();
    put_u32_be(out, 0);

    const std::size_t bytesPerRow = static_cast<std::size_t>((image.size.w + 15) / 16) * 2;
    std::vector<std::uint8_t> row(bytesPerRow);
    for (int y = 0; y < image.size.h; ++y) {
        const std::uint8_t* pixels = &image.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(image.size.w)];
        for (int plane = 0; plane < planes; ++plane) {
            fill_plane_row(row, pixels, image.size.w, plane, o.base);
            const std::vector<std::uint8_t> packed = byterun1(row.data(), row.size());
            out.insert(out.end(), packed.begin(), packed.end());
        }
    }

    const auto length = static_cast<std::uint32_t>(out.size() - lengthAt - 4);
    patch_u32_be(out, lengthAt, length);
    if ((length & 1) != 0) {
        out.push_back(0);
    }
}

} // namespace

int planes_for(const Options& o)
{
    const int pens = o.base + o.colors;
    int planes = 1;
    while ((1 << planes) < pens) {
        ++planes;
    }
    return planes;
}

std::vector<std::uint8_t> byterun1(const std::uint8_t* row, std::size_t n)
{
    std::vector<std::uint8_t> out;
    std::size_t i = 0;
    while (i < n) {
        const std::size_t run = repeat_length(row, n, i);
        if (run >= 2) {
            out.push_back(static_cast<std::uint8_t>(257 - run));
            out.push_back(row[i]);
            i += run;
            continue;
        }
        const std::size_t literal = literal_length(row, n, i);
        out.push_back(static_cast<std::uint8_t>(literal - 1));
        out.insert(out.end(), row + i, row + i + literal);
        i += literal;
    }
    return out;
}

std::vector<std::uint8_t> write(const IndexedImage& image, const Options& o)
{
    const int planes = planes_for(o);
    std::vector<std::uint8_t> out;
    put_tag(out, "FORM");
    put_u32_be(out, 0);
    put_tag(out, "ILBM");
    write_bmhd(out, image.size, planes, o);
    write_cmap(out, image.palette, planes, o);
    write_body(out, image, planes, o);
    patch_u32_be(out, 4, static_cast<std::uint32_t>(out.size() - 8));
    return out;
}

} // namespace fujinet::image::ilbm
