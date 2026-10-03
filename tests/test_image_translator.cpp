// tests/test_image_translator.cpp
#include "doctest.h"
#include "image_fixtures.h"

#include "fujinet/io/devices/image_content_translator.h"
#include "fujinet/io/devices/network_translation.h"

#include <cstdint>
#include <string>
#include <vector>

using fujinet::io::ContentTranslationType;
using fujinet::io::IContentTranslator;
using fujinet::io::ImageContentTranslator;
using fujinet::io::StatusCode;
using fujinet::io::TranslationConfig;
using fujinet::tests::image::kPng2x1;
using fujinet::tests::image::kPng8192x1;
using fujinet::tests::image::kPng8193x1;

namespace {

// Large enough for every fixture; the cap tests set their own.
constexpr std::uint32_t kTestMaxPixels = 4096u * 4096u;

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
    ImageContentTranslator t(kTestMaxPixels);
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
    ImageContentTranslator t(kTestMaxPixels);
    CHECK(t.configure(image_config("colors=99")) == StatusCode::InvalidRequest);
    REQUIRE(t.configure(image_config("")) == StatusCode::Ok);
    const std::uint8_t junk[] = {1, 2, 3, 4};
    REQUIRE(t.append_body(junk, sizeof(junk)) == StatusCode::Ok);
    CHECK(t.finalize() == StatusCode::InvalidRequest);
}

TEST_CASE("ImageTranslator honours the pixel cap it is constructed with")
{
    // The 2x1 PNG has 2 pixels: a cap of 2 allows it, a cap of 1 does not.
    ImageContentTranslator atCap(2);
    REQUIRE(atCap.configure(image_config("")) == StatusCode::Ok);
    REQUIRE(atCap.append_body(kPng2x1, sizeof(kPng2x1)) == StatusCode::Ok);
    CHECK(atCap.finalize() == StatusCode::Ok);
    CHECK(atCap.max_pixels() == 2);

    ImageContentTranslator belowSize(1);
    REQUIRE(belowSize.configure(image_config("")) == StatusCode::Ok);
    REQUIRE(belowSize.append_body(kPng2x1, sizeof(kPng2x1)) == StatusCode::Ok);
    CHECK(belowSize.finalize() == StatusCode::Unsupported);
    CHECK(belowSize.translated_size() == 0);
}

TEST_CASE("ImageTranslator accepts fmt=ilbm and rejects other formats at configure")
{
    ImageContentTranslator t(kTestMaxPixels);
    CHECK(t.configure(image_config("fmt=ilbm")) == StatusCode::Ok);
    CHECK(t.configure(image_config("fmt=ilbm,bits=8")) == StatusCode::Ok);
    CHECK(t.configure(image_config("fmt=png")) == StatusCode::InvalidRequest);
    CHECK(t.configure(image_config("bits=9")) == StatusCode::InvalidRequest);
}

TEST_CASE("ImageTranslator: stb_image refuses a side over 8192 even under the pixel cap")
{
    // 8193 pixels is well under the cap; only STBI_MAX_DIMENSIONS stops it.
    // stb_image rejects the header itself, so this reads as an undecodable
    // body (stbi_info() tries every format and keeps only the last reason).
    ImageContentTranslator wide(kTestMaxPixels);
    REQUIRE(wide.configure(image_config("")) == StatusCode::Ok);
    REQUIRE(wide.append_body(kPng8193x1, sizeof(kPng8193x1)) == StatusCode::Ok);
    CHECK(wide.finalize() == StatusCode::InvalidRequest);

    ImageContentTranslator widest(kTestMaxPixels);
    REQUIRE(widest.configure(image_config("")) == StatusCode::Ok);
    REQUIRE(widest.append_body(kPng8192x1, sizeof(kPng8192x1)) == StatusCode::Ok);
    CHECK(widest.finalize() == StatusCode::Ok);
}

namespace {

// Records the calls the IContentTranslator default translate() makes.
class RecordingTranslator final : public IContentTranslator {
public:
    StatusCode configure(const TranslationConfig&) override { return StatusCode::Ok; }
    void reset() override { calls += "reset;"; }
    StatusCode append_body(const std::uint8_t* data, std::size_t len) override
    {
        calls += "append;";
        body.assign(data, data + len);
        return StatusCode::Ok;
    }
    StatusCode finalize() override
    {
        calls += "finalize;";
        return StatusCode::Ok;
    }
    std::uint64_t translated_size() const override { return 0; }
    StatusCode read(std::uint32_t, std::uint8_t*, std::size_t, std::uint16_t& actual, bool& eof) const override
    {
        actual = 0;
        eof = true;
        return StatusCode::Ok;
    }

    std::string calls;
    std::vector<std::uint8_t> body;
};

std::vector<std::uint8_t> read_everything(const IContentTranslator& t)
{
    std::vector<std::uint8_t> out(static_cast<std::size_t>(t.translated_size()));
    std::uint16_t n = 0;
    bool eof = false;
    REQUIRE(t.read(0, out.data(), out.size(), n, eof) == StatusCode::Ok);
    REQUIRE(n == out.size());
    return out;
}

} // namespace

TEST_CASE("IContentTranslator::translate defaults to reset, append_body, finalize")
{
    RecordingTranslator t;
    const std::uint8_t body[] = {'{', '}'};
    CHECK(t.translate(body, sizeof(body)) == StatusCode::Ok);
    CHECK(t.calls == "reset;append;finalize;");
    CHECK(t.body == std::vector<std::uint8_t>{'{', '}'});
}

TEST_CASE("ImageTranslator::translate decodes the caller's buffer without copying it in")
{
    ImageContentTranslator viaAppend(kTestMaxPixels);
    REQUIRE(viaAppend.configure(image_config("colors=2,mode=gray,dither=none")) == StatusCode::Ok);
    REQUIRE(viaAppend.append_body(kPng2x1, sizeof(kPng2x1)) == StatusCode::Ok);
    REQUIRE(viaAppend.finalize() == StatusCode::Ok);

    ImageContentTranslator viaView(kTestMaxPixels);
    REQUIRE(viaView.configure(image_config("colors=2,mode=gray,dither=none")) == StatusCode::Ok);
    REQUIRE(viaView.translate(kPng2x1, sizeof(kPng2x1)) == StatusCode::Ok);
    CHECK(read_everything(viaView) == read_everything(viaAppend));

    // A second translate on the same translator replaces the output.
    const std::uint8_t junk[] = {1, 2, 3, 4};
    CHECK(viaView.translate(junk, sizeof(junk)) == StatusCode::InvalidRequest);
    CHECK(viaView.translated_size() == 0);
}
