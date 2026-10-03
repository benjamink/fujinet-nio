// tests/test_image_translator.cpp
#include "doctest.h"
#include "image_fixtures.h"

#include "fujinet/io/devices/image_content_translator.h"
#include "fujinet/io/devices/network_translation.h"

#include <cstdint>
#include <string>

using fujinet::io::ContentTranslationType;
using fujinet::io::ImageContentTranslator;
using fujinet::io::StatusCode;
using fujinet::io::TranslationConfig;
using fujinet::tests::image::kPng2x1;

namespace {

TranslationConfig image_config(const char* selector)
{
    TranslationConfig config;
    config.type = ContentTranslationType::Image;
    config.selector = selector;
    return config;
}

} // namespace

TEST_CASE("ImageTranslator converts PNG to ILBM")
{
    ImageContentTranslator t;
    REQUIRE(t.configure(image_config("colors=2,mode=gray,dither=none")) == StatusCode::Ok);
    REQUIRE(t.append_body(kPng2x1, sizeof(kPng2x1)) == StatusCode::Ok);
    REQUIRE(t.finalize() == StatusCode::Ok);

    std::uint8_t buf[256];
    std::uint16_t n = 0;
    bool eof = false;
    REQUIRE(t.read(0, buf, sizeof(buf), n, eof) == StatusCode::Ok);
    CHECK(eof);
    CHECK(n == t.translated_size());
    CHECK(std::string(reinterpret_cast<const char*>(buf), 4) == "FORM");
    CHECK(buf[20] == 0);    // BMHD width = 2
    CHECK(buf[21] == 2);
}

TEST_CASE("ImageTranslator rejects bad selector and garbage body")
{
    ImageContentTranslator t;
    CHECK(t.configure(image_config("colors=99")) == StatusCode::InvalidRequest);
    REQUIRE(t.configure(image_config("")) == StatusCode::Ok);
    const std::uint8_t junk[] = {1, 2, 3, 4};
    REQUIRE(t.append_body(junk, sizeof(junk)) == StatusCode::Ok);
    CHECK(t.finalize() == StatusCode::InvalidRequest);
}

TEST_CASE("ImageTranslator rejects images above pixel cap")
{
    ImageContentTranslator t;
    t.set_max_pixels_for_test(1);       // the 2x1 PNG has 2 pixels
    REQUIRE(t.configure(image_config("")) == StatusCode::Ok);
    REQUIRE(t.append_body(kPng2x1, sizeof(kPng2x1)) == StatusCode::Ok);
    CHECK(t.finalize() == StatusCode::Unsupported);
}
