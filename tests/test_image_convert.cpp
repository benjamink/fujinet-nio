// tests/test_image_convert.cpp
#include "doctest.h"
#include "fujinet/io/devices/image_convert.h"
using namespace fujinet::io::image;

TEST_CASE("ImageConvert: selector defaults and full parse") {
    Options o;
    CHECK(parse_selector("", o));
    CHECK(o.w == 640); CHECK(o.h == 400); CHECK(o.colors == 16); CHECK(o.base == 0);
    CHECK(parse_selector("w=624,h=190,colors=12,base=4,par=1:2,dither=none,mode=gray,up=1", o));
    CHECK(o.w == 624); CHECK(o.h == 190); CHECK(o.colors == 12); CHECK(o.base == 4);
    CHECK(o.parX == 1); CHECK(o.parY == 2); CHECK(!o.dither); CHECK(o.mode == Options::Gray); CHECK(o.upscale);
    CHECK(planes_for(o) == 4);
}

TEST_CASE("ImageConvert: selector rejects bad input") {
    Options o;
    CHECK_FALSE(parse_selector("w=0", o));
    CHECK_FALSE(parse_selector("colors=40", o));
    CHECK_FALSE(parse_selector("base=28,colors=8", o));   // 36 > 32
    CHECK_FALSE(parse_selector("bogus=1", o));
    CHECK_FALSE(parse_selector("w=10,w=20", o));
    CHECK_FALSE(parse_selector("par=0:1", o));
}

TEST_CASE("ImageConvert: fit_size keeps aspect with pixel-aspect correction") {
    // par=1:2: each output row is displayed twice as tall, so 300 square source rows need 150 output rows at scale 1.
    // Box 640x100: sx = 640*1024/740 = 885, sy = 100*2*1024/300 = 682 -> s = 682 (height-limited).
    Options o; parse_selector("w=640,h=100,par=1:2", o);
    Size s = fit_size(740, 300, o);
    CHECK(s.w == 492); CHECK(s.h == 99);     // 740*682/1024 = 492, 300*682/2048 = 99 (integer truncation)
    Options q; parse_selector("w=640,h=200,par=1:2", q);
    Size u = fit_size(740, 300, q);          // width-limited: s = 885
    CHECK(u.w == 639); CHECK(u.h == 129);
    Options p; parse_selector("w=640,h=400", p);
    Size t = fit_size(100, 50, p);           // no upscale by default
    CHECK(t.w == 100); CHECK(t.h == 50);
}

TEST_CASE("ImageConvert: byterun1 encodes runs and literals") {
    const std::uint8_t row[] = {0,0,0,0,1,2,3,3};
    auto e = byterun1(row, sizeof row);
    const std::vector<std::uint8_t> want = {0xFD,0x00, 0x01,1,2, 0xFF,3}; // run4 of 0, lit 2, run2 of 3
    CHECK(e == want);
}

TEST_CASE("ImageConvert: gray palette spans black to white") {
    std::vector<std::uint8_t> rgb = {0,0,0, 255,255,255, 128,128,128, 64,64,64};
    Options o; parse_selector("colors=4,mode=gray", o);
    auto pal = make_palette(rgb, o);
    REQUIRE(pal.size() == 4);
    CHECK(pal.front().r == 0); CHECK(pal.back().r == 255);
}

TEST_CASE("ImageConvert: ILBM header, CMAP base offset and indices") {
    std::vector<std::uint8_t> rgb(16*16*3, 255); rgb[0]=rgb[1]=rgb[2]=0;   // 16x16, first pixel black
    Options o; parse_selector("w=16,h=16,colors=2,base=4,mode=gray,dither=none", o);
    auto pal = make_palette(rgb, o);
    auto idx = map_pixels(rgb, {16,16}, pal, o);
    CHECK(idx[0] == 4); CHECK(idx[1] == 5);
    auto f = write_ilbm(idx, {16,16}, pal, o);
    CHECK(std::string(f.begin(), f.begin()+4) == "FORM");
    CHECK(std::string(f.begin()+8, f.begin()+12) == "ILBM");
    CHECK(std::string(f.begin()+12, f.begin()+16) == "BMHD");
    CHECK(f[28] == 3);                       // nPlanes for base 4 + 2 colours = 6 pens -> 3 planes
    CHECK(f[30] == 1);                       // compression = ByteRun1
    std::uint32_t formLen = (f[4]<<24)|(f[5]<<16)|(f[6]<<8)|f[7];
    CHECK(formLen + 8 == f.size());
}
