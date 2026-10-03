// Unit tests for alphaflow::market::SyntheticFeed.

#include <alphaflow/market/synthetic_feed.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {

using alphaflow::market::FeedId;
using alphaflow::market::SyntheticFeed;

}  // namespace

TEST_CASE("the same seed yields the same tick content", "[market][feed]") {
    SyntheticFeed a(FeedId::A, 4, 0.03, 12'345);
    SyntheticFeed b(FeedId::A, 4, 0.03, 12'345);

    for (int i = 0; i < 1'000; ++i) {
        const auto ta = a.next();
        const auto tb = b.next();
        REQUIRE(ta.id == tb.id);
        REQUIRE(ta.value == tb.value);
        REQUIRE(ta.seq == tb.seq);
        REQUIRE(ta.feed == tb.feed);
    }
}

TEST_CASE("different seeds diverge", "[market][feed]") {
    SyntheticFeed a(FeedId::A, 4, 0.03, 1);
    SyntheticFeed b(FeedId::A, 4, 0.03, 2);
    REQUIRE(a.next().value != b.next().value);
}

TEST_CASE("instruments cycle in order", "[market][feed]") {
    SyntheticFeed feed(FeedId::B, 3, 0.03, 7);
    for (std::uint64_t i = 0; i < 30; ++i) {
        const auto tick = feed.next();
        REQUIRE(tick.id == static_cast<std::uint32_t>(i % 3));
        REQUIRE(tick.seq == i + 1);
        REQUIRE(tick.feed == FeedId::B);
    }
    REQUIRE(feed.produced() == 30);
}
