// Unit tests for alphaflow::metrics::MetricsHub.
//
// The hub is the live Metrics thread: it samples every registered interval
// recorder into a per-stage aggregate without blocking the recording threads.

#include <alphaflow/metrics/metrics_hub.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

using alphaflow::metrics::IntervalRecorder;
using alphaflow::metrics::MetricsHub;
using alphaflow::metrics::Stage;

}  // namespace

TEST_CASE("the hub aggregates each stage independently", "[metrics][hub]") {
    IntervalRecorder rebuild;
    IntervalRecorder rfq;

    MetricsHub hub;
    REQUIRE(hub.add(Stage::Rebuild, rebuild));
    REQUIRE(hub.add(Stage::Rfq, rfq));

    rebuild.record(100);
    rfq.record(200);
    rfq.record(300);

    hub.sample_once();

    const auto rebuild_profile = hub.profile(Stage::Rebuild);
    const auto rfq_profile = hub.profile(Stage::Rfq);

    REQUIRE(rebuild_profile.stage == Stage::Rebuild);
    REQUIRE(rebuild_profile.count == 1);
    REQUIRE(rfq_profile.count == 2);
}

TEST_CASE("an unregistered stage reports an empty profile", "[metrics][hub]") {
    MetricsHub hub;
    hub.sample_once();

    const auto profile = hub.profile(Stage::Ingest);
    REQUIRE(profile.stage == Stage::Ingest);
    REQUIRE(profile.count == 0);
}

TEST_CASE("the hub refuses to exceed its capacity", "[metrics][hub]") {
    IntervalRecorder a;
    IntervalRecorder b;
    IntervalRecorder c;
    IntervalRecorder d;
    IntervalRecorder e;

    MetricsHub hub;
    REQUIRE(hub.add(Stage::Ingest, a));
    REQUIRE(hub.add(Stage::Rebuild, b));
    REQUIRE(hub.add(Stage::Rfq, c));
    REQUIRE(hub.add(Stage::Ingest, d));
    REQUIRE_FALSE(hub.add(Stage::Rfq, e));  // capacity reached
}
