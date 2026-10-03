// tests/test_image_pipeline.cpp
#include "doctest.h"

#include "fujinet/image/image_pipeline.h"
#include "fujinet/image/output_format.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace fujinet::image;

TEST_CASE("ImagePipeline: selector defaults and full parse")
{
    Options o;
    CHECK(parse_selector("", o));
    CHECK(o.w == 640);
    CHECK(o.h == 400);
    CHECK(o.colors == 16);
    CHECK(o.base == 0);

    CHECK(parse_selector("w=624,h=190,colors=12,base=4,par=1:2,dither=none,mode=gray,up=1", o));
    CHECK(o.w == 624);
    CHECK(o.h == 190);
    CHECK(o.colors == 12);
    CHECK(o.base == 4);
    CHECK(o.parX == 1);
    CHECK(o.parY == 2);
    CHECK_FALSE(o.dither);
    CHECK(o.mode == ColourMode::Gray);
    CHECK(o.upscale);
}

TEST_CASE("ImagePipeline: selector rejects bad input")
{
    Options o;
    CHECK_FALSE(parse_selector("w=0", o));
    CHECK_FALSE(parse_selector("colors=40", o));
    CHECK_FALSE(parse_selector("base=28,colors=8", o));   // 36 > 32
    CHECK_FALSE(parse_selector("bogus=1", o));
    CHECK_FALSE(parse_selector("w=10,w=20", o));
    CHECK_FALSE(parse_selector("par=0:1", o));
    CHECK_FALSE(parse_selector(",", o));
    CHECK_FALSE(parse_selector("w", o));
}

TEST_CASE("ImagePipeline: fit_size keeps aspect with pixel-aspect correction")
{
    // par=1:2: each output row is displayed twice as tall, so 300 square source
    // rows need 150 output rows at scale 1.
    // Box 640x100: sx = 640*1024/740 = 885, sy = 100*2*1024/300 = 682, so
    // s = 682 (height-limited).
    Options o;
    REQUIRE(parse_selector("w=640,h=100,par=1:2", o));
    const Size s = fit_size(740, 300, o);
    CHECK(s.w == 492);      // 740*682/1024
    CHECK(s.h == 99);       // 300*682/2048, truncated

    Options q;
    REQUIRE(parse_selector("w=640,h=200,par=1:2", q));
    const Size u = fit_size(740, 300, q);   // width-limited: s = 885
    CHECK(u.w == 639);
    CHECK(u.h == 129);

    Options p;
    REQUIRE(parse_selector("w=640,h=400", p));
    const Size t = fit_size(100, 50, p);    // no upscale by default
    CHECK(t.w == 100);
    CHECK(t.h == 50);
}

TEST_CASE("ImagePipeline: gray palette spans black to white")
{
    const std::vector<std::uint8_t> rgb = {0, 0, 0, 255, 255, 255, 128, 128, 128, 64, 64, 64};
    Options o;
    REQUIRE(parse_selector("colors=4,mode=gray", o));
    const auto palette = make_palette(rgb, o);
    REQUIRE(palette.size() == 4);
    CHECK(palette.front().r == 0);
    CHECK(palette.back().r == 255);
}

TEST_CASE("ImagePipeline: map_pixels returns palette indices without the base")
{
    std::vector<std::uint8_t> rgb(16 * 16 * 3, 255);
    rgb[0] = 0;
    rgb[1] = 0;
    rgb[2] = 0;
    Options o;
    REQUIRE(parse_selector("w=16,h=16,colors=2,base=4,mode=gray,dither=none", o));
    const auto palette = make_palette(rgb, o);
    const auto pixels = map_pixels(rgb, {16, 16}, palette, o);
    CHECK(pixels[0] == 0);
    CHECK(pixels[1] == 1);
}

// ---------------------------------------------------------------------------
// fmt and bits
// ---------------------------------------------------------------------------

TEST_CASE("ImagePipeline: fmt accepts ilbm and rejects everything else")
{
    Options o;
    REQUIRE(parse_selector("", o));
    REQUIRE(o.format != nullptr);
    CHECK(o.format->name == "ilbm");
    CHECK(o.format == &default_output_format());
    CHECK(parse_selector("fmt=ilbm", o));
    CHECK(o.format == find_output_format("ilbm"));
    CHECK(parse_selector("w=320,fmt=ilbm,colors=8", o));

    CHECK_FALSE(parse_selector("fmt=", o));
    CHECK_FALSE(parse_selector("fmt=ILBM", o));
    CHECK_FALSE(parse_selector("fmt=png", o));
    CHECK_FALSE(parse_selector("fmt=bbc", o));
    CHECK_FALSE(parse_selector("fmt=ilbm,fmt=ilbm", o));
}

TEST_CASE("ImagePipeline: bits defaults to 4 for ilbm and accepts 1..8")
{
    Options o;
    REQUIRE(parse_selector("", o));
    CHECK(o.bits == 4);
    CHECK(find_output_format("ilbm")->defaultBits == 4);
    for (int bits = 1; bits <= 8; ++bits) {
        CAPTURE(bits);
        REQUIRE(parse_selector("bits=" + std::to_string(bits), o));
        CHECK(o.bits == bits);
    }

    CHECK_FALSE(parse_selector("bits=0", o));
    CHECK_FALSE(parse_selector("bits=9", o));
    CHECK_FALSE(parse_selector("bits=", o));
    CHECK_FALSE(parse_selector("bits=x", o));
    CHECK_FALSE(parse_selector("bits=4,bits=4", o));
}

TEST_CASE("ImagePipeline: the output format sets the pen limit, wherever fmt comes")
{
    const OutputFormat* ilbm = find_output_format("ilbm");
    REQUIRE(ilbm != nullptr);
    CHECK(ilbm->maxPens == 32);
    CHECK(ilbm->allowsBase);
    CHECK(find_output_format("png") == nullptr);

    Options o;
    CHECK(parse_selector("colors=32", o));
    CHECK_FALSE(parse_selector("colors=33", o));            // a byte holds 256, ILBM 32
    CHECK(parse_selector("colors=8,base=24,fmt=ilbm", o));
    CHECK_FALSE(parse_selector("colors=8,base=25,fmt=ilbm", o));   // checked after fmt
    CHECK_FALSE(parse_selector("colors=257", o));
}

TEST_CASE("ImagePipeline: snap_channel spaces 2^bits levels evenly")
{
    for (int v = 0; v <= 255; ++v) {
        CAPTURE(v);
        CHECK(snap_channel(v, 4) == ((v + 8) / 17) * 17);     // the OCS grid
        CHECK(snap_channel(v, 8) == v);
        CHECK(snap_channel(v, 1) == (v >= 128 ? 255 : 0));
        const int two = snap_channel(v, 2);
        CHECK((two == 0 || two == 85 || two == 170 || two == 255));
        CHECK(std::abs(two - v) <= 42);
    }
    for (int bits = 1; bits <= 8; ++bits) {
        CAPTURE(bits);
        CHECK(snap_channel(0, bits) == 0);
        CHECK(snap_channel(255, bits) == 255);
        for (int v = 0; v <= 255; ++v) {
            const int s = snap_channel(v, bits);
            CHECK(snap_channel(s, bits) == s);      // already on the grid
        }
    }
}

namespace {

// 16x16 RGB image made of four 8x8 blocks of the given colours.
std::vector<std::uint8_t> four_blocks(const Rgb (&colours)[4])
{
    std::vector<std::uint8_t> rgb;
    rgb.reserve(16 * 16 * 3);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const Rgb& c = colours[(y / 8) * 2 + (x / 8)];
            rgb.push_back(c.r);
            rgb.push_back(c.g);
            rgb.push_back(c.b);
        }
    }
    return rgb;
}

bool same_colour(const Rgb& a, const Rgb& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

bool contains_colour(const std::vector<Rgb>& palette, const Rgb& c)
{
    for (const Rgb& p : palette) {
        if (same_colour(p, c)) {
            return true;
        }
    }
    return false;
}

const Rgb kBlocks[4] = {{200, 30, 60}, {20, 180, 90}, {40, 70, 220}, {230, 210, 20}};

} // namespace

TEST_CASE("ImagePipeline: median cut gives distinct colours snapped to bits")
{
    const auto rgb = four_blocks(kBlocks);
    for (int bits = 1; bits <= 8; ++bits) {
        CAPTURE(bits);
        Options o;
        REQUIRE(parse_selector("colors=4,bits=" + std::to_string(bits), o));
        const auto palette = make_palette(rgb, o);
        REQUIRE(palette.size() == 4);
        for (std::size_t i = 0; i < palette.size(); ++i) {
            const Rgb& p = palette[i];
            CAPTURE(i);
            CHECK(snap_channel(p.r, bits) == p.r);
            CHECK(snap_channel(p.g, bits) == p.g);
            CHECK(snap_channel(p.b, bits) == p.b);
            if (bits >= 2) {
                CHECK_FALSE((p.r == p.g && p.g == p.b));    // not a gray
                for (std::size_t j = 0; j < i; ++j) {
                    CHECK_FALSE(same_colour(palette[j], p));
                }
            }
        }
    }
}

TEST_CASE("ImagePipeline: median cut keeps the OCS bin colours at bits=4 and exact colours at bits=8")
{
    const auto rgb = four_blocks(kBlocks);
    Options o;

    REQUIRE(parse_selector("colors=4", o));
    const auto ocs = make_palette(rgb, o);
    CHECK(contains_colour(ocs, {204, 17, 51}));     // 200,30,60 in 4-bit bins (12,1,3) * 17
    CHECK(contains_colour(ocs, {238, 221, 17}));    // 230,210,20 -> (14,13,1) * 17

    REQUIRE(parse_selector("colors=4,bits=8", o));
    const auto full = make_palette(rgb, o);
    for (const Rgb& c : kBlocks) {
        CHECK(contains_colour(full, c));
    }
}

TEST_CASE("ImagePipeline: gray palette is snapped to bits")
{
    const std::vector<std::uint8_t> rgb = {0, 0, 0, 255, 255, 255};
    Options o;
    REQUIRE(parse_selector("colors=4,mode=gray,bits=2", o));
    const auto two = make_palette(rgb, o);
    REQUIRE(two.size() == 4);
    CHECK(two[0].r == 0);
    CHECK(two[1].r == 85);
    CHECK(two[2].r == 170);
    CHECK(two[3].r == 255);

    REQUIRE(parse_selector("colors=3,mode=gray,bits=8", o));
    const auto full = make_palette(rgb, o);
    REQUIRE(full.size() == 3);
    CHECK(full[1].r == 127);        // 1*255/2, not snapped to 119 or 136
}

namespace {

// Horizontal gray ramp, `w` pixels wide and `h` rows tall.
std::vector<std::uint8_t> gray_ramp(int w, int h)
{
    std::vector<std::uint8_t> rgb;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto v = static_cast<std::uint8_t>(x * 255 / (w - 1));
            rgb.push_back(v);
            rgb.push_back(v);
            rgb.push_back(v);
        }
    }
    return rgb;
}

int transitions(const std::vector<std::uint8_t>& row)
{
    int n = 0;
    for (std::size_t i = 1; i < row.size(); ++i) {
        if (row[i] != row[i - 1]) {
            ++n;
        }
    }
    return n;
}

} // namespace

TEST_CASE("ImagePipeline: dither=none is nearest colour, fs diffuses the error")
{
    const int w = 64;
    const int h = 4;
    const auto rgb = gray_ramp(w, h);

    Options none;
    REQUIRE(parse_selector("colors=2,mode=gray,dither=none", none));
    const auto palette = make_palette(rgb, none);
    REQUIRE(palette.size() == 2);
    const auto plain = map_pixels(rgb, {w, h}, palette, none);
    const auto again = map_pixels(rgb, {w, h}, palette, none);
    CHECK(plain == again);

    // Every pixel takes whichever of black and white is nearer: one edge per row.
    for (int x = 0; x < w; ++x) {
        const int v = rgb[static_cast<std::size_t>(x) * 3];
        CAPTURE(x);
        CHECK(plain[static_cast<std::size_t>(x)] == (v > 127 ? 1 : 0));
    }
    const std::vector<std::uint8_t> plainRow(plain.begin(), plain.begin() + w);
    CHECK(transitions(plainRow) == 1);

    Options fs;
    REQUIRE(parse_selector("colors=2,mode=gray,dither=fs", fs));
    const auto dithered = map_pixels(rgb, {w, h}, palette, fs);
    CHECK(dithered != plain);
    const std::vector<std::uint8_t> ditheredRow(dithered.begin(), dithered.begin() + w);
    CHECK(transitions(ditheredRow) > 4);

    // Error diffusion keeps the average brightness of the ramp: about half
    // the pixels are white.
    int white = 0;
    for (const std::uint8_t p : dithered) {
        white += p;
    }
    CHECK(white > w * h * 2 / 5);
    CHECK(white < w * h * 3 / 5);
}

TEST_CASE("ImagePipeline: decode_to_indexed reports the source size")
{
    // 2x1 PNG: black, white.
    const std::uint8_t png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x7B, 0x40, 0xE8,
        0xDD, 0x00, 0x00, 0x00, 0x0F, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x60, 0x60, 0x60, 0xF8,
        0xFF, 0xFF, 0x3F, 0x00, 0x06, 0x01, 0x02, 0xFE, 0x02, 0xB2, 0x39, 0xAE, 0x00, 0x00, 0x00, 0x00,
        0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
    };
    Options o;
    REQUIRE(parse_selector("up=1,w=64,h=32", o));
    IndexedImage image;
    Size source{};
    REQUIRE(decode_to_indexed(png, sizeof(png), 100, o, image, source) == fujinet::io::StatusCode::Ok);
    CHECK(source.w == 2);
    CHECK(source.h == 1);
    CHECK(image.size.w == 64);
    CHECK(image.size.h == 32);
    CHECK(image.pixels.size() == 64u * 32u);

    // A failed decode still reports what it learnt.
    Size tooBig{};
    CHECK(decode_to_indexed(png, sizeof(png), 1, o, image, tooBig) == fujinet::io::StatusCode::Unsupported);
    CHECK(tooBig.w == 2);
    CHECK(tooBig.h == 1);
}

// ---------------------------------------------------------------------------
// NearestColour: the fast matcher must agree with nearest_colour() exactly
// ---------------------------------------------------------------------------

namespace {

// Every (r, g, b) on a grid with the given step, plus the extremes.
template <typename Fn>
void for_each_colour(int step, Fn fn)
{
    for (int r = 0; r <= 255; r += step) {
        for (int g = 0; g <= 255; g += step) {
            for (int b = 0; b <= 255; b += step) {
                fn(r, g, b);
            }
        }
    }
    fn(255, 255, 255);
}

int count_mismatches(const std::vector<Rgb>& palette, int step)
{
    NearestColour matcher(palette);
    int mismatches = 0;
    for_each_colour(step, [&](int r, int g, int b) {
        if (matcher.find(r, g, b) != nearest_colour(palette, r, g, b)) {
            ++mismatches;
        }
    });
    return mismatches;
}

} // namespace

TEST_CASE("NearestColour matches nearest_colour for gray palettes, including ties and repeats")
{
    const std::vector<std::uint8_t> rgb = {0, 0, 0, 255, 255, 255};
    for (int colors : {2, 3, 4, 12, 16, 32}) {
        for (int bits : {1, 2, 4, 8}) {
            CAPTURE(colors);
            CAPTURE(bits);
            Options o;
            REQUIRE(parse_selector("mode=gray,colors=" + std::to_string(colors) + ",bits=" + std::to_string(bits), o));
            // bits=1 with several colours repeats entries: the lowest index must win.
            CHECK(count_mismatches(make_palette(rgb, o), 3) == 0);
        }
    }
    // Hand-made gray palettes: unsorted, repeated and evenly tied entries.
    CHECK(count_mismatches({{200, 200, 200}, {10, 10, 10}, {100, 100, 100}}, 3) == 0);
    CHECK(count_mismatches({{0, 0, 0}, {0, 0, 0}, {255, 255, 255}, {255, 255, 255}}, 3) == 0);
    CHECK(count_mismatches({{0, 0, 0}, {100, 100, 100}, {200, 200, 200}}, 1) == 0);
}

TEST_CASE("NearestColour matches nearest_colour for colour palettes, cached or not")
{
    const Rgb blocks[4] = {{200, 30, 60}, {20, 180, 90}, {40, 70, 220}, {230, 210, 20}};
    std::vector<std::uint8_t> rgb;
    for (int i = 0; i < 64; ++i) {
        const Rgb& c = blocks[i % 4];
        rgb.push_back(static_cast<std::uint8_t>(c.r + i));
        rgb.push_back(static_cast<std::uint8_t>(c.g + i / 2));
        rgb.push_back(static_cast<std::uint8_t>(c.b));
    }
    for (const char* selector : {"colors=4", "colors=16,bits=8", "colors=32,bits=2"}) {
        CAPTURE(selector);
        Options o;
        REQUIRE(parse_selector(selector, o));
        const auto palette = make_palette(rgb, o);
        CHECK(count_mismatches(palette, 5) == 0);
    }
    // Ties between different colours, and a repeated query served from the cache.
    const std::vector<Rgb> tied = {{0, 0, 0}, {0, 0, 0}, {255, 0, 0}, {0, 0, 255}, {128, 128, 128}};
    CHECK(count_mismatches(tied, 5) == 0);
    NearestColour matcher(tied);
    CHECK(matcher.find(10, 10, 10) == nearest_colour(tied, 10, 10, 10));
    CHECK(matcher.find(10, 10, 10) == nearest_colour(tied, 10, 10, 10));
}
