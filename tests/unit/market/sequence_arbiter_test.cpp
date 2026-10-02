// Unit tests for alphaflow::market::SequenceArbiter.
//
// The arbiter knows nothing about sockets or locks; it maps a stream of
// per-feed ticks to a single delivered stream (ARCHITECTURE.md §6.2, ADR-014):
// duplicate suppression on a line, gap detection, promotion of the peer when
// the active line gaps, and restoration of the preferred line once it recovers.

#include <alphaflow/market/sequence_arbiter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {

using alphaflow::market::FeedId;
using alphaflow::market::SequenceArbiter;
using alphaflow::market::Tick;

Tick tick(FeedId feed, std::uint64_t sequence) {
    Tick value;
    value.id = 1;
    value.value = 0.0;
    value.seq = sequence;
    value.feed = feed;
    return value;
}

}  // namespace

TEST_CASE("the preferred line is delivered in order", "[market][arbiter]") {
    SequenceArbiter arbiter;

    REQUIRE(arbiter.observe(tick(FeedId::A, 1)));
    REQUIRE(arbiter.observe(tick(FeedId::A, 2)));
    REQUIRE(arbiter.observe(tick(FeedId::A, 3)));

    REQUIRE(arbiter.active() == FeedId::A);
    REQUIRE(arbiter.last_sequence(FeedId::A) == 3);
    REQUIRE(arbiter.duplicate_count() == 0);
}

TEST_CASE("duplicates and out-of-order ticks are dropped", "[market][arbiter]") {
    SequenceArbiter arbiter;

    REQUIRE(arbiter.observe(tick(FeedId::A, 1)));
    REQUIRE(arbiter.observe(tick(FeedId::A, 2)));
    REQUIRE_FALSE(arbiter.observe(tick(FeedId::A, 2)));  // duplicate
    REQUIRE_FALSE(arbiter.observe(tick(FeedId::A, 1)));  // stale

    REQUIRE(arbiter.duplicate_count() == 2);
    REQUIRE(arbiter.last_sequence(FeedId::A) == 2);
}

TEST_CASE("a gap marks the line stale and promotes the peer", "[market][arbiter]") {
    SequenceArbiter arbiter;

    REQUIRE(arbiter.observe(tick(FeedId::A, 1)));
    REQUIRE(arbiter.observe(tick(FeedId::A, 2)));
    REQUIRE_FALSE(arbiter.observe(tick(FeedId::A, 5)));  // 3 and 4 missing

    REQUIRE(arbiter.gap_count(FeedId::A) == 1);
    REQUIRE(arbiter.active() == FeedId::B);

    // The peer now carries the stream.
    REQUIRE(arbiter.observe(tick(FeedId::B, 1)));
    REQUIRE(arbiter.observe(tick(FeedId::B, 2)));
}

TEST_CASE("the preferred line is restored once it recovers", "[market][arbiter]") {
    SequenceArbiter arbiter;

    REQUIRE(arbiter.observe(tick(FeedId::A, 1)));
    REQUIRE_FALSE(arbiter.observe(tick(FeedId::A, 3)));  // gap; A goes stale
    REQUIRE(arbiter.active() == FeedId::B);

    // A's next in-order tick (4, following the gap tick 3) recovers it.
    REQUIRE(arbiter.observe(tick(FeedId::A, 4)));
    REQUIRE(arbiter.active() == FeedId::A);
}

TEST_CASE("bootstrap uses whichever line speaks first", "[market][arbiter]") {
    SequenceArbiter arbiter;

    // B arrives before A has produced anything; it is used until A is healthy.
    REQUIRE(arbiter.observe(tick(FeedId::B, 10)));
    REQUIRE(arbiter.active() == FeedId::B);

    // A speaks; the preferred line takes over.
    REQUIRE(arbiter.observe(tick(FeedId::A, 1)));
    REQUIRE(arbiter.active() == FeedId::A);
}

TEST_CASE("a gap on the backup line is counted but does not change the active line",
          "[market][arbiter]") {
    SequenceArbiter arbiter;

    REQUIRE(arbiter.observe(tick(FeedId::A, 1)));
    REQUIRE_FALSE(arbiter.observe(tick(FeedId::B, 1)));  // B is not the active line
    // B gaps while A is active: the gap is recorded, A stays active.
    REQUIRE_FALSE(arbiter.observe(tick(FeedId::B, 4)));

    REQUIRE(arbiter.gap_count(FeedId::B) == 1);
    REQUIRE(arbiter.active() == FeedId::A);
}
