// tests/test_image_pipeline.cpp
#include "doctest.h"

#include "fujinet/io/devices/image_pipeline.h"

#include <cstdint>
#include <vector>

using namespace fujinet::io::image;

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
