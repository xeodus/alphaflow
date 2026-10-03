// Reliability tests (M3): active-active determinism, A/B failover, and recovery
// by replay.

#include <alphaflow/concurrency/latest_value_cache.hpp>
#include <alphaflow/concurrency/spsc_ring.hpp>
#include <alphaflow/core/date.hpp>
#include <alphaflow/curve/bootstrap.hpp>
#include <alphaflow/curve/calendar.hpp>
#include <alphaflow/curve/day_count.hpp>
#include <alphaflow/curve/discount_curve.hpp>
#include <alphaflow/market/arbiter.hpp>
#include <alphaflow/market/replay_log.hpp>
#include <alphaflow/market/synthetic_feed.hpp>
#include <alphaflow/market/tick.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using alphaflow::concurrency::LatestValueCache;
using alphaflow::concurrency::SpscRing;
using alphaflow::core::Date;
using alphaflow::curve::BootstrapSpec;
using alphaflow::curve::Calendar;
using alphaflow::curve::DayCount;
using alphaflow::curve::DiscountCurve;
using alphaflow::curve::Frequency;
using alphaflow::curve::OisPillar;
using alphaflow::market::Arbiter;
using alphaflow::market::FeedId;
using alphaflow::market::InstrumentId;
using alphaflow::market::ReplayLog;
using alphaflow::market::SyntheticFeed;
using alphaflow::market::Tick;

constexpr std::size_t kPillars = 5;
constexpr std::size_t kInput = 64;
constexpr std::size_t kCache = 8;
constexpr std::size_t kNotify = 64;
const Date kSpot{2024, 1, 8};

using ArbiterType = Arbiter<kCache, kNotify, kInput>;

Date maturity(std::size_t index) {
    return kSpot.add_months(static_cast<int>(12 * (index + 1)));
}

std::array<OisPillar, kPillars> pillars_from(const std::array<double, kPillars>& rates) {
    std::array<OisPillar, kPillars> pillars{};
    for (std::size_t i = 0; i < kPillars; ++i) {
        pillars[i] = OisPillar{maturity(i), rates[i]};
    }
    return pillars;
}

DiscountCurve bootstrap_from(const std::array<double, kPillars>& rates) {
    BootstrapSpec spec;
    spec.reference = kSpot;
    spec.frequency = Frequency::Annual;
    return alphaflow::curve::bootstrap(pillars_from(rates), Calendar::united_states(), spec).curve;
}

std::array<double, kPillars> rates_from_feed(std::uint64_t seed) {
    SyntheticFeed feed(FeedId::A, static_cast<std::uint32_t>(kPillars), 0.03, seed);
    std::array<double, kPillars> rates{};
    for (int i = 0; i < 1'000; ++i) {
        const Tick tick = feed.next();
        rates[tick.id] = tick.value;
    }
    return rates;
}

}  // namespace

TEST_CASE("two active-active replicas produce byte-identical curves",
          "[reliability][active-active]") {
    const DiscountCurve a = bootstrap_from(rates_from_feed(7));
    const DiscountCurve b = bootstrap_from(rates_from_feed(7));

    REQUIRE(a.count == b.count);
    REQUIRE(std::memcmp(a.times.data(), b.times.data(), a.count * sizeof(double)) == 0);
    REQUIRE(std::memcmp(a.log_dfs.data(), b.log_dfs.data(), a.count * sizeof(double)) == 0);
}

TEST_CASE("a gap on the active line hands the stream to the peer",
          "[reliability][failover]") {
    ArbiterType::InputRing ring_a;
    ArbiterType::InputRing ring_b;
    ArbiterType::Cache cache;
    ArbiterType::NotificationRing notifications;
    ArbiterType arbiter(ring_a, ring_b, cache, notifications);

    auto tick = [](FeedId feed, std::uint64_t seq, double value) {
        Tick t;
        t.id = 0;
        t.value = value;
        t.seq = seq;
        t.feed = feed;
        return t;
    };

    REQUIRE(ring_a.try_push(tick(FeedId::A, 1, 0.030)));
    REQUIRE(ring_a.try_push(tick(FeedId::A, 2, 0.031)));
    REQUIRE(ring_a.try_push(tick(FeedId::A, 5, 0.099)));  // gap: 3,4 missing
    REQUIRE(ring_b.try_push(tick(FeedId::B, 1, 0.040)));
    arbiter.drain_once();

    REQUIRE(arbiter.sequences().gap_count(FeedId::A) == 1);
    REQUIRE(arbiter.sequences().active() == FeedId::B);
    REQUIRE(cache.load(0) == 0.040);  // the peer's value won
}

TEST_CASE("recovery by replay reproduces the live curve", "[reliability][recovery]") {
    const char* path = "recovery_test.bin";

    // Live: feed a deterministic stream, keep the last value per instrument.
    SyntheticFeed feed(FeedId::A, static_cast<std::uint32_t>(kPillars), 0.03, 21);
    ReplayLog log;
    REQUIRE(log.open(path));
    std::array<double, kPillars> live_rates{};
    for (int i = 0; i < 500; ++i) {
        const Tick tick = feed.next();
        live_rates[tick.id] = tick.value;
        REQUIRE(log.append(tick));
    }
    log.close();

    // Recovery: replay the log and rebuild the quote set.
    std::array<Tick, 500> replayed{};
    REQUIRE(ReplayLog::read_all(path, replayed.data(), replayed.size()) == 500);
    std::array<double, kPillars> replayed_rates{};
    for (const Tick& tick : replayed) {
        replayed_rates[tick.id] = tick.value;
    }

    const DiscountCurve live = bootstrap_from(live_rates);
    const DiscountCurve recovered = bootstrap_from(replayed_rates);
    REQUIRE(live.count == recovered.count);
    REQUIRE(std::memcmp(live.log_dfs.data(), recovered.log_dfs.data(),
                        live.count * sizeof(double)) == 0);

    std::remove(path);
}
