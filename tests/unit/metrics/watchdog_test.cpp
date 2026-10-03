// Unit tests for the heartbeat watchdog.

#include <alphaflow/metrics/watchdog.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {

using alphaflow::metrics::Heartbeat;
using alphaflow::metrics::Watchdog;

}  // namespace

TEST_CASE("the watchdog detects a stalled heartbeat", "[metrics][watchdog]") {
    Heartbeat heartbeat;
    std::uint64_t last_seen = 0;

    heartbeat.beat();
    REQUIRE(Watchdog::observe(heartbeat, last_seen));  // advanced

    REQUIRE_FALSE(Watchdog::observe(heartbeat, last_seen));  // stalled

    heartbeat.beat();
    REQUIRE(Watchdog::observe(heartbeat, last_seen));  // alive again
}
