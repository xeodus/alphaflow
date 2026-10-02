// Unit tests for alphaflow::market::Arbiter.
//
// The arbiter is the sole writer of the quote cache (ADR-014): it drains the
// two feed rings, arbitrates, and fans each accepted tick into the cache and a
// change-notification ring. These tests pin the fan-in, the rejection of the
// non-active line, failover on a gap, and the no-lost-change guarantee when the
// notification ring is full.

#include <alphaflow/market/arbiter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

namespace {

using alphaflow::market::Arbiter;
using alphaflow::market::FeedId;
using alphaflow::market::InstrumentId;
using alphaflow::market::Tick;

constexpr std::size_t kInput = 64;
constexpr std::size_t kCache = 8;

using TestArbiter = Arbiter<kCache, 16, kInput>;

Tick tick(FeedId feed, std::uint64_t sequence, InstrumentId id, double value) {
    Tick result;
    result.id = id;
    result.value = value;
    result.seq = sequence;
    result.feed = feed;
    return result;
}

}  // namespace

TEST_CASE("the arbiter fans active-line ticks into the cache and notifications",
          "[market][arbiter]") {
    TestArbiter::InputRing ring_a;
    TestArbiter::InputRing ring_b;
    TestArbiter::Cache cache;
    TestArbiter::NotificationRing notifications;
    TestArbiter arbiter(ring_a, ring_b, cache, notifications);

    REQUIRE(ring_a.try_push(tick(FeedId::A, 1, 3, 0.05)));
    REQUIRE(ring_a.try_push(tick(FeedId::A, 2, 3, 0.06)));
    arbiter.drain_once();

    REQUIRE(cache.epoch(3) == 2);
    REQUIRE(cache.load(3) == 0.06);

    InstrumentId id = 0;
    REQUIRE(notifications.try_pop(id));
    REQUIRE(id == 3);
    REQUIRE(notifications.try_pop(id));
    REQUIRE(id == 3);
    REQUIRE_FALSE(notifications.try_pop(id));
}

TEST_CASE("the arbiter ignores the non-active line", "[market][arbiter]") {
    TestArbiter::InputRing ring_a;
    TestArbiter::InputRing ring_b;
    TestArbiter::Cache cache;
    TestArbiter::NotificationRing notifications;
    TestArbiter arbiter(ring_a, ring_b, cache, notifications);

    REQUIRE(ring_a.try_push(tick(FeedId::A, 1, 0, 0.1)));
    REQUIRE(ring_b.try_push(tick(FeedId::B, 1, 0, 0.2)));
    arbiter.drain_once();

    REQUIRE(cache.epoch(0) == 1);   // only A's tick reached the cache
    REQUIRE(cache.load(0) == 0.1);

    InstrumentId id = 0;
    REQUIRE(notifications.try_pop(id));
    REQUIRE(id == 0);
    REQUIRE_FALSE(notifications.try_pop(id));
}

TEST_CASE("a gap on the active line hands the stream to the peer",
          "[market][arbiter]") {
    TestArbiter::InputRing ring_a;
    TestArbiter::InputRing ring_b;
    TestArbiter::Cache cache;
    TestArbiter::NotificationRing notifications;
    TestArbiter arbiter(ring_a, ring_b, cache, notifications);

    REQUIRE(ring_a.try_push(tick(FeedId::A, 1, 0, 0.1)));
    REQUIRE(ring_a.try_push(tick(FeedId::A, 3, 0, 0.3)));  // gap: 2 missing
    REQUIRE(ring_b.try_push(tick(FeedId::B, 1, 0, 0.9)));
    arbiter.drain_once();

    REQUIRE(cache.epoch(0) == 2);   // A:1 and B:1; A:3 was dropped
    REQUIRE(cache.load(0) == 0.9);
    REQUIRE(arbiter.sequences().gap_count(FeedId::A) == 1);
    REQUIRE(arbiter.sequences().active() == FeedId::B);
}

TEST_CASE("a full notification ring never loses the cached value",
          "[market][arbiter]") {
    // A two-slot notification ring forces coalescing.
    Arbiter<kCache, 2, kInput>::InputRing ring_a;
    Arbiter<kCache, 2, kInput>::InputRing ring_b;
    Arbiter<kCache, 2, kInput>::Cache cache;
    Arbiter<kCache, 2, kInput>::NotificationRing notifications;
    Arbiter<kCache, 2, kInput> arbiter(ring_a, ring_b, cache, notifications);

    for (std::uint64_t sequence = 1; sequence <= 5; ++sequence) {
        REQUIRE(ring_a.try_push(tick(FeedId::A, sequence, 1, static_cast<double>(sequence))));
    }
    arbiter.drain_once();

    // Every update reached the cache; the ring kept what it could.
    REQUIRE(cache.epoch(1) == 5);
    REQUIRE(cache.load(1) == 5.0);

    InstrumentId id = 0;
    std::size_t delivered = 0;
    while (notifications.try_pop(id)) {
        ++delivered;
    }
    REQUIRE(delivered == 2);
}
