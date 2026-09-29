// Unit tests for alphaflow::platform::Clock.
//
// The clock is the foundation of every latency measurement in the system, so
// its contract is tested before it is implemented:
//
//   * initialize() is idempotent and observable via is_initialized();
//   * the raw counter and the nanosecond view are both monotonic;
//   * ticks_to_ns() is consistent with now_ns() (both derive from the same
//     calibrated monotonic counter);
//   * the clock advances by a plausible amount across a known sleep;
//   * the reported source is one of the known hardware clocks.

#include <alphaflow/platform/clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

namespace {

using alphaflow::platform::Clock;
using alphaflow::platform::ClockSource;
using alphaflow::platform::Nanos;
using alphaflow::platform::Ticks;

constexpr Nanos kMillisecond = 1'000'000;

}  // namespace

TEST_CASE("clock initialize is idempotent and observable", "[platform][clock]") {
    Clock::initialize();
    Clock::initialize();
    REQUIRE(Clock::is_initialized());
}

TEST_CASE("monotonic nanoseconds never go backwards", "[platform][clock]") {
    const Nanos a = Clock::now_ns();
    const Nanos b = Clock::now_ns();
    const Nanos c = Clock::now_ns();

    REQUIRE(b >= a);
    REQUIRE(c >= b);
}

TEST_CASE("raw ticks never go backwards", "[platform][clock]") {
    const Ticks a = Clock::now_ticks();
    const Ticks b = Clock::now_ticks();

    REQUIRE(b >= a);
}

TEST_CASE("ticks_to_ns is consistent with the now_ns window", "[platform][clock]") {
    const Nanos before = Clock::now_ns();
    const Ticks sample = Clock::now_ticks();
    const Nanos after = Clock::now_ns();

    const Nanos converted = Clock::ticks_to_ns(sample);

    REQUIRE(converted >= before);
    REQUIRE(converted <= after);
}

TEST_CASE("ticks_to_ns is monotonic in its argument", "[platform][clock]") {
    REQUIRE(Clock::ticks_to_ns(1'000) >= Clock::ticks_to_ns(999));
    REQUIRE(Clock::ticks_to_ns(0) >= 0);
}

TEST_CASE("clock advances across a sleep", "[platform][clock]") {
    const Nanos before = Clock::now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const Nanos after = Clock::now_ns();

    const Nanos elapsed = after - before;

    REQUIRE(elapsed >= 4 * kMillisecond);
    REQUIRE(elapsed <= 500 * kMillisecond);
}

TEST_CASE("clock reports a known source", "[platform][clock]") {
    const ClockSource source = Clock::source();

    REQUIRE((source == ClockSource::MachAbsoluteTime ||
             source == ClockSource::InvariantTsc ||
             source == ClockSource::MonotonicRaw));
}
