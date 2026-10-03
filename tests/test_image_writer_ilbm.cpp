// tests/test_image_writer_ilbm.cpp
#include "doctest.h"

#include "fujinet/image/ilbm_writer.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace fujinet::image;
using namespace fujinet::image::ilbm;

namespace {

std::uint32_t read_u32_be(const std::vector<std::uint8_t>& f, std::size_t at)
{
    return (static_cast<std::uint32_t>(f[at]) << 24)
         | (static_cast<std::uint32_t>(f[at + 1]) << 16)
         | (static_cast<std::uint32_t>(f[at + 2]) << 8)
         | static_cast<std::uint32_t>(f[at + 3]);
}

} // namespace

TEST_CASE("ImageWriterIlbm: planes cover base plus colours")
{
    Options o;
    REQUIRE(parse_selector("colors=12,base=4", o));
    CHECK(planes_for(o) == 4);
    REQUIRE(parse_selector("colors=2", o));
    CHECK(planes_for(o) == 1);
    REQUIRE(parse_selector("colors=32", o));
    CHECK(planes_for(o) == 5);
}

TEST_CASE("ImageWriterIlbm: byterun1 encodes runs and literals")
{
    const std::uint8_t row[] = {0, 0, 0, 0, 1, 2, 3, 3};
    const auto packed = byterun1(row, sizeof(row));
    // run of four 0s, literal {1,2}, run of two 3s
    const std::vector<std::uint8_t> want = {0xFD, 0x00, 0x01, 1, 2, 0xFF, 3};
    CHECK(packed == want);
}

TEST_CASE("ImageWriterIlbm: header, CMAP base offset and pens")
{
    IndexedImage image;
    image.size = {16, 16};
    image.pixels.assign(16 * 16, 1);
    image.pixels[0] = 0;
    image.palette = {{0, 0, 0}, {255, 255, 255}};
    Options o;
    REQUIRE(parse_selector("w=16,h=16,colors=2,base=4", o));

    const auto f = ilbm::write(image, o);
    CHECK(std::string(f.begin(), f.begin() + 4) == "FORM");
    CHECK(std::string(f.begin() + 8, f.begin() + 12) == "ILBM");
    CHECK(std::string(f.begin() + 12, f.begin() + 16) == "BMHD");
    CHECK(f[28] == 3);      // nPlanes: base 4 + 2 colours = 6 pens
    CHECK(f[30] == 1);      // compression = ByteRun1
    CHECK(read_u32_be(f, 4) + 8 == f.size());

    // CMAP follows BMHD (8 + 20 bytes from offset 12): pens 4 and 5 hold the palette.
    const std::size_t cmap = 12 + 8 + 20;
    CHECK(std::string(f.begin() + cmap, f.begin() + cmap + 4) == "CMAP");
    CHECK(read_u32_be(f, cmap + 4) == 3 * 8);
    const std::size_t pen4 = cmap + 8 + 4 * 3;
    CHECK(f[pen4] == 0);
    CHECK(f[pen4 + 3] == 255);
    CHECK(f[cmap + 8] == 0);    // pen 0 is unused and black

    // First BODY row, plane 2 (value 4): every pixel has pen 4 or 5, so the
    // bit is set everywhere: one run of two 0xFF bytes.
    const std::size_t body = cmap + 8 + 3 * 8;
    CHECK(std::string(f.begin() + body, f.begin() + body + 4) == "BODY");
    // Plane 0 of row 0: pixel 0 is pen 4 (bit clear), the rest pen 5 (bit set).
    CHECK(f[body + 8] == 0x01);     // literal of 2 bytes
    CHECK(f[body + 9] == 0x7F);
    CHECK(f[body + 10] == 0xFF);
}
