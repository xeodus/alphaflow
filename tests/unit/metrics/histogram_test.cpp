// Unit tests for alphaflow::metrics::Histogram / LatencyRecorder.
//
// The metrics stage makes the project's numbers honest (ARCHITECTURE.md §11,
// ADR-012). Two things matter and are tested here:
//
//   * the histogram itself: counts, extremes, mean, percentiles, reset, merge;
//   * the span contract: every stage has a written-down interval, and the RFQ
//     span is fixed to the socket-to-socket definition. A number without its
//     span is not a claim, so the span strings are pinned as tests.

#include <alphaflow/metrics/histogram.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

namespace {

using alphaflow::metrics::Histogram;
using alphaflow::metrics::LatencyRecorder;
using alphaflow::metrics::Nanos;
using alphaflow::metrics::Stage;

// HdrHistogram reports the highest equivalent value of a bucket, so a recorded
// value is reported at or just above itself, never below.
bool quantized_close(Nanos actual, Nanos expected) {
    const double difference = static_cast<double>(actual) - static_cast<double>(expected);
    const double tolerance = static_cast<double>(expected) * 1e-3 + 1.0;
    return difference >= 0.0 && difference <= tolerance;
}

}  // namespace

TEST_CASE("an empty histogram reports nothing", "[metrics][histogram]") {
    Histogram histogram;

    REQUIRE(histogram.valid());
    REQUIRE(histogram.count() == 0);

    const auto profile = histogram.profile(Stage::Rfq);
    REQUIRE(profile.count == 0);
    REQUIRE(profile.max_ns == 0);
}

TEST_CASE("a histogram tracks count, extremes and mean", "[metrics][histogram]") {
    Histogram histogram;
    histogram.record(1000);
    histogram.record(3000);
    histogram.record(2000);

    REQUIRE(histogram.count() == 3);
    REQUIRE(histogram.min() == 1000);
    REQUIRE(quantized_close(histogram.max(), 3000));
    REQUIRE(histogram.mean() > 1999.0);
    REQUIRE(histogram.mean() < 2001.0);
}

TEST_CASE("percentiles are ordered and approximately correct", "[metrics][histogram]") {
    Histogram histogram;
    for (Nanos value = 1; value <= 100'000; ++value) {
        histogram.record(value);
    }

    REQUIRE(histogram.count() == 100'000);
    REQUIRE(histogram.min() == 1);
    REQUIRE(quantized_close(histogram.max(), 100'000));

    const Nanos p50 = histogram.value_at_percentile(50.0);
    const Nanos p90 = histogram.value_at_percentile(90.0);
    const Nanos p99 = histogram.value_at_percentile(99.0);

    REQUIRE(p50 >= 49'500);
    REQUIRE(p50 <= 50'500);
    REQUIRE(p90 >= 89'000);
    REQUIRE(p90 <= 91'000);
    REQUIRE(p99 >= 98'000);
    REQUIRE(p99 <= 100'000);

    REQUIRE(p50 <= p90);
    REQUIRE(p90 <= p99);
}

TEST_CASE("record_elapsed records the difference", "[metrics][histogram]") {
    Histogram histogram;
    histogram.record_elapsed(1'000, 1'500);

    REQUIRE(histogram.count() == 1);
    REQUIRE(histogram.min() == 500);
    REQUIRE(histogram.max() == 500);
}

TEST_CASE("reset clears the histogram", "[metrics][histogram]") {
    Histogram histogram;
    histogram.record(1234);
    REQUIRE(histogram.count() == 1);

    histogram.reset();
    REQUIRE(histogram.count() == 0);
}

TEST_CASE("accumulate merges another histogram", "[metrics][histogram]") {
    Histogram left;
    Histogram right;

    for (Nanos value = 1; value <= 100; ++value) {
        left.record(value);
    }
    for (Nanos value = 101; value <= 200; ++value) {
        right.record(value);
    }

    left.accumulate(right);

    REQUIRE(left.count() == 200);
    REQUIRE(left.min() == 1);
    REQUIRE(left.max() == 200);
}

TEST_CASE("a recorder profile carries its stage and count", "[metrics][histogram]") {
    LatencyRecorder recorder(Stage::Rfq);
    recorder.record(1'000);
    recorder.record(2'000);
    recorder.record(3'000);

    const auto profile = recorder.profile();
    REQUIRE(profile.stage == Stage::Rfq);
    REQUIRE(profile.count == 3);
    REQUIRE(profile.min_ns == 1'000);
    REQUIRE(quantized_close(profile.max_ns, 3'000));
}

TEST_CASE("every stage has a distinct name and a written-down span contract",
          "[metrics][histogram]") {
    REQUIRE(alphaflow::metrics::stage_name(Stage::Ingest) == "ingest");
    REQUIRE(alphaflow::metrics::stage_name(Stage::Rebuild) == "rebuild");
    REQUIRE(alphaflow::metrics::stage_name(Stage::Rfq) == "rfq");

    // The RFQ span is the socket-to-socket interval, not the in-process handler
    // time. Pinned so a refactor cannot quietly shrink it.
    REQUIRE(alphaflow::metrics::stage_span(Stage::Rfq) ==
            "request bytes read off socket -> response bytes handed to socket");
    REQUIRE_FALSE(alphaflow::metrics::stage_span(Stage::Ingest).empty());
    REQUIRE_FALSE(alphaflow::metrics::stage_span(Stage::Rebuild).empty());
    REQUIRE(alphaflow::metrics::stage_span(Stage::Ingest) !=
            alphaflow::metrics::stage_span(Stage::Rebuild));
}
