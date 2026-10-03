// Determinism tests (ARCHITECTURE.md §13.1): the same inputs must produce
// byte-identical output, and replaying the same log must reproduce it.

#include <alphaflow/concurrency/spsc_ring.hpp>
#include <alphaflow/core/date.hpp>
#include <alphaflow/curve/bootstrap.hpp>
#include <alphaflow/curve/calendar.hpp>
#include <alphaflow/curve/day_count.hpp>
#include <alphaflow/curve/discount_curve.hpp>
#include <alphaflow/market/replay_log.hpp>
#include <alphaflow/market/synthetic_feed.hpp>
#include <alphaflow/market/tick.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

using alphaflow::core::Date;
using alphaflow::curve::BootstrapSpec;
using alphaflow::curve::Calendar;
using alphaflow::curve::DayCount;
using alphaflow::curve::DiscountCurve;
using alphaflow::curve::Frequency;
using alphaflow::curve::OisPillar;
using alphaflow::market::FeedId;
using alphaflow::market::ReplayLog;
using alphaflow::market::SyntheticFeed;
using alphaflow::market::Tick;

constexpr std::size_t kPillars = 5;
const Date kSpot{2024, 1, 8};

Date maturity(std::size_t index) {
    return kSpot.add_months(static_cast<int>(12 * (index + 1)));
}

}  // namespace

TEST_CASE("the same inputs produce a byte-identical curve", "[determinism]") {
    SyntheticFeed feed(FeedId::A, static_cast<std::uint32_t>(kPillars), 0.03, 42);

    std::array<double, kPillars> rates{};
    for (int i = 0; i < 1'000; ++i) {
        const Tick tick = feed.next();
        rates[tick.id] = tick.value;
    }

    std::array<OisPillar, kPillars> pillars{};
    for (std::size_t i = 0; i < kPillars; ++i) {
        pillars[i] = OisPillar{maturity(i), rates[i]};
    }

    BootstrapSpec spec;
    spec.reference = kSpot;
    spec.frequency = Frequency::Annual;

    const DiscountCurve a = alphaflow::curve::bootstrap(pillars, Calendar::united_states(), spec).curve;
    const DiscountCurve b = alphaflow::curve::bootstrap(pillars, Calendar::united_states(), spec).curve;

    REQUIRE(a.count == b.count);
    REQUIRE(std::memcmp(a.times.data(), b.times.data(), a.count * sizeof(double)) == 0);
    REQUIRE(std::memcmp(a.log_dfs.data(), b.log_dfs.data(), a.count * sizeof(double)) == 0);
}

TEST_CASE("replaying the same log reproduces the same ticks", "[determinism]") {
    const char* path = "determinism_test.bin";

    SyntheticFeed feed(FeedId::A, 4, 0.03, 99);
    ReplayLog log;
    REQUIRE(log.open(path));
    for (int i = 0; i < 100; ++i) {
        REQUIRE(log.append(feed.next()));
    }
    log.close();

    std::array<Tick, 100> first{};
    std::array<Tick, 100> second{};
    REQUIRE(ReplayLog::read_all(path, first.data(), first.size()) == 100);
    REQUIRE(ReplayLog::read_all(path, second.data(), second.size()) == 100);
    REQUIRE(std::memcmp(first.data(), second.data(), sizeof(Tick) * 100) == 0);

    std::remove(path);
}
