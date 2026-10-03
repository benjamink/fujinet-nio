// tests/test_image_golden.cpp
//
// Pins the Image translator's output byte for byte. The hashes were taken from
// the first ILBM-only implementation, before the pipeline/writer split, so a
// selector without fmt= or bits= must keep producing exactly these files.

#include "doctest.h"
#include "image_fixtures.h"

#include "fujinet/io/devices/image_content_translator.h"
#include "fujinet/io/devices/network_translation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using fujinet::io::ContentTranslationType;
using fujinet::io::ImageContentTranslator;
using fujinet::io::StatusCode;
using fujinet::io::TranslationConfig;
using fujinet::tests::image::fnv1a;
using fujinet::tests::image::kPngColour16x12;

std::vector<std::uint8_t> translate_colour_fixture(const char* selector)
{
    ImageContentTranslator t;
    TranslationConfig config;
    config.type = ContentTranslationType::Image;
    config.selector = selector;
    REQUIRE(t.configure(config) == StatusCode::Ok);
    REQUIRE(t.append_body(kPngColour16x12, sizeof(kPngColour16x12)) == StatusCode::Ok);
    REQUIRE(t.finalize() == StatusCode::Ok);

    std::vector<std::uint8_t> out(static_cast<std::size_t>(t.translated_size()));
    std::uint32_t offset = 0;
    while (offset < out.size()) {
        std::uint16_t got = 0;
        bool eof = false;
        REQUIRE(t.read(offset, out.data() + offset, out.size() - offset, got, eof) == StatusCode::Ok);
        offset += got;
        if (eof) {
            break;
        }
    }
    return out;
}

} // namespace

TEST_CASE("ImageGolden: default selector output is unchanged")
{
    const auto out = translate_colour_fixture("");
    CHECK(out.size() == 238);
    CHECK(fnv1a(out) == 0xBDA7F5DFu);
}

TEST_CASE("ImageGolden: upscaled 5-colour dithered output is unchanged")
{
    const auto out = translate_colour_fixture("up=1,colors=5");
    CHECK(out.size() == 39930);
    CHECK(fnv1a(out) == 0x95302CF1u);
}

TEST_CASE("ImageGolden: gray undithered output is unchanged")
{
    const auto out = translate_colour_fixture("mode=gray,dither=none,up=1,w=48,h=36");
    CHECK(out.size() == 936);
    CHECK(fnv1a(out) == 0x12C033BEu);
}

TEST_CASE("ImageGolden: par 1:2 with 12 colours output is unchanged")
{
    const auto out = translate_colour_fixture("w=40,h=30,up=1,colors=12,par=1:2");
    CHECK(out.size() == 478);
    CHECK(fnv1a(out) == 0x076BBAE6u);
}

TEST_CASE("ImageGolden: explicit fmt=ilbm,bits=4 matches the default output")
{
    const auto out = translate_colour_fixture("fmt=ilbm,bits=4");
    CHECK(out.size() == 238);
    CHECK(fnv1a(out) == 0xBDA7F5DFu);
    CHECK(translate_colour_fixture("up=1,colors=5,bits=4,fmt=ilbm") == translate_colour_fixture("up=1,colors=5"));
}

TEST_CASE("ImageGolden: bits changes the palette and so the output")
{
    CHECK(translate_colour_fixture("bits=8") != translate_colour_fixture(""));
    CHECK(translate_colour_fixture("bits=2") != translate_colour_fixture(""));
}
