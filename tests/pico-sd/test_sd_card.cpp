#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "fake_sd_card.h"

#include <numeric>

using fujinet::platform::pico::detail::SdCard;
using fakesd::Card;
using fakesd::Options;

namespace {

constexpr std::uint32_t kRun = 10000000;

std::vector<std::uint8_t> pattern(std::size_t n, unsigned seed) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>((i * 131 + seed * 17 + (i >> 8)) & 0xFF);
    return v;
}

} // namespace

TEST_CASE("an SDHC card initialises: SPI mode at a slow clock, then the run clock, capacity from the CSD") {
    Card card;
    SdCard sd(card, kRun);
    REQUIRE(sd.init());
    CHECK(sd.ready());
    CHECK(sd.high_capacity());
    CHECK(sd.sector_count() == 131072);
    CHECK(card.max_clock_before_ready <= 400000); // the spec's limit until the card is initialised
    CHECK(card.clock == kRun);
    CHECK(card.illegal == 0);
    CHECK(card.cmd16 == 0);
}

TEST_CASE("an SDSC v2 card uses byte addressing and CMD16") {
    Options o; o.sdhc = false; o.sectors = 65536;
    Card card(o);
    SdCard sd(card, kRun);
    REQUIRE(sd.init());
    CHECK_FALSE(sd.high_capacity());
    CHECK(card.cmd16 == 1); // byte-addressed cards are told the block length; SDHC are not
    CHECK(sd.sector_count() == 65536);
    auto data = pattern(512, 3);
    REQUIRE(sd.write(100, data.data(), 1));
    CHECK(std::equal(data.begin(), data.end(), card.image().begin() + 100 * 512)); // the right byte address
    std::vector<std::uint8_t> back(512);
    REQUIRE(sd.read(100, back.data(), 1));
    CHECK(back == data);
}

TEST_CASE("a version 1 card (CMD8 rejected) initialises and reports its CSD v1 capacity") {
    Options o; o.sdhc = false; o.v1 = true; o.sectors = 4096;
    Card card(o);
    SdCard sd(card, kRun);
    REQUIRE(sd.init());
    CHECK(sd.sector_count() == 4096);
    CHECK_FALSE(sd.high_capacity());
}

TEST_CASE("slow cards: many ACMD41 polls are tolerated, a card that never becomes ready is given up on") {
    { Options o; o.init_polls = 150; Card card(o); SdCard sd(card, kRun); CHECK(sd.init()); }
    Options o; o.never_ready = true;
    Card card(o);
    SdCard sd(card, kRun);
    const auto start = card.now_ms();
    CHECK_FALSE(sd.init());
    CHECK_FALSE(sd.ready());
    CHECK(card.now_ms() - start < 3500); // bounded: a missing card must not hang startup
}

TEST_CASE("no card: init fails quickly and cleanly") {
    Options o; o.absent = true;
    Card card(o);
    SdCard sd(card, kRun);
    const auto start = card.now_ms();
    CHECK_FALSE(sd.init());
    CHECK(card.now_ms() - start < 500);
    std::uint8_t b[512];
    CHECK_FALSE(sd.read(0, b, 1)); // not ready: refuses without touching the bus
    CHECK_FALSE(sd.write(0, b, 1));
}

TEST_CASE("single and multi block reads and writes are byte exact, including the last sector") {
    Card card;
    SdCard sd(card, kRun);
    REQUIRE(sd.init());
    const std::uint32_t last = sd.sector_count() - 1;

    auto one = pattern(512, 1);
    REQUIRE(sd.write(7, one.data(), 1));
    auto many = pattern(512 * 9, 2);
    REQUIRE(sd.write(1000, many.data(), 9));
    auto tail = pattern(512 * 2, 3);
    REQUIRE(sd.write(last - 1, tail.data(), 2));
    CHECK(card.writes == 12);

    std::vector<std::uint8_t> back(512 * 9);
    REQUIRE(sd.read(7, back.data(), 1));
    CHECK(std::equal(one.begin(), one.end(), back.begin()));
    REQUIRE(sd.read(1000, back.data(), 9));
    CHECK(back == many);
    std::vector<std::uint8_t> t(1024);
    REQUIRE(sd.read(last - 1, t.data(), 2));
    CHECK(t == tail);
    // neighbours untouched
    CHECK(card.image()[6 * 512] == 0);
    CHECK(card.image()[1009 * 512] == 0);
    CHECK(sd.ready());
}

TEST_CASE("many consecutive multi-block transfers keep the card in step") {
    Card card;
    SdCard sd(card, kRun);
    REQUIRE(sd.init());
    for (unsigned round = 0; round < 20; ++round) {
        const std::uint32_t lba = round * 37;
        const std::uint32_t n = 2 + round % 7;
        auto data = pattern(512ull * n, round);
        REQUIRE(sd.write(lba, data.data(), n));
        std::vector<std::uint8_t> back(data.size());
        REQUIRE(sd.read(lba, back.data(), n));
        REQUIRE(back == data);
    }
    CHECK(card.illegal == 0);
}

TEST_CASE("out of range and empty requests are refused or no-ops without disturbing the card") {
    Card card;
    SdCard sd(card, kRun);
    REQUIRE(sd.init());
    std::vector<std::uint8_t> b(1024);
    const auto before = card.commands;
    CHECK_FALSE(sd.read(sd.sector_count(), b.data(), 1));
    CHECK_FALSE(sd.read(sd.sector_count() - 1, b.data(), 2));
    CHECK_FALSE(sd.write(sd.sector_count() - 1, b.data(), 2));
    CHECK_FALSE(sd.read(0xFFFFFFFFu, b.data(), 2));
    CHECK(sd.read(5, b.data(), 0));
    CHECK(card.commands == before);
    CHECK(sd.ready());
}

TEST_CASE("a read that times out waiting for the data token fails, and init() recovers the card") {
    Card card;
    SdCard sd(card, kRun);
    REQUIRE(sd.init());
    auto data = pattern(512, 9);
    REQUIRE(sd.write(3, data.data(), 1));

    card.options().no_read_token = true;
    std::vector<std::uint8_t> b(512);
    const auto start = card.now_ms();
    CHECK_FALSE(sd.read(3, b.data(), 1));
    CHECK(card.now_ms() - start < 400);
    CHECK_FALSE(sd.ready());

    card.options().no_read_token = false;
    REQUIRE(sd.init());
    REQUIRE(sd.read(3, b.data(), 1));
    CHECK(b == data);
}

TEST_CASE("a rejected write, and a card stuck busy, are reported as failures") {
    {
        Card card;
        SdCard sd(card, kRun);
        REQUIRE(sd.init());
        card.options().reject_writes = true;
        auto d = pattern(512, 1);
        CHECK_FALSE(sd.write(1, d.data(), 1));
        CHECK_FALSE(sd.ready());
    }
    {
        Card card;
        SdCard sd(card, kRun);
        REQUIRE(sd.init());
        card.options().stuck_busy = true;
        auto d = pattern(1024, 1);
        const auto start = card.now_ms();
        CHECK_FALSE(sd.write(1, d.data(), 2));
        CHECK(card.now_ms() - start < 3000); // gives up
        CHECK_FALSE(sd.ready());
    }
}

TEST_CASE("sync waits out a slow programming cycle") {
    Options o; o.busy_bytes = 4000; // long busy at 10 MHz is ~3 ms; at the init clock far longer
    Card card(o);
    SdCard sd(card, kRun);
    REQUIRE(sd.init());
    auto d = pattern(512, 4);
    REQUIRE(sd.write(9, d.data(), 1));
    CHECK(sd.sync());
    CHECK(sd.ready());
}
