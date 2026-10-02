// Unit tests for alphaflow::metrics::IntervalRecorder.
//
// The interval recorder is the lock-free buffer the live Metrics thread drains
// (ARCHITECTURE.md §11): recording threads write without blocking, and a single
// sampling thread periodically harvests the interval into an aggregate.

#include <alphaflow/metrics/interval_recorder.hpp>

#include <alphaflow/metrics/histogram.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

using alphaflow::metrics::Histogram;
using alphaflow::metrics::IntervalRecorder;

}  // namespace

TEST_CASE("sampling harvests the interval into an aggregate", "[metrics][interval]") {
    IntervalRecorder recorder;
    REQUIRE(recorder.valid());

    recorder.record(1'000);
    recorder.record(2'000);
    recorder.record(3'000);

    Histogram aggregate;
    recorder.sample_into(aggregate);

    REQUIRE(aggregate.count() == 3);
    REQUIRE(aggregate.min() == 1'000);
    REQUIRE(aggregate.max() >= 3'000);
}

TEST_CASE("successive intervals accumulate into the same aggregate",
          "[metrics][interval]") {
    IntervalRecorder recorder;
    Histogram aggregate;

    recorder.record(100);
    recorder.sample_into(aggregate);
    recorder.record(200);
    recorder.sample_into(aggregate);

    REQUIRE(aggregate.count() == 2);
}

TEST_CASE("recording and sampling are safe concurrently", "[metrics][interval]") {
    constexpr int kCount = 20'000;

    IntervalRecorder recorder;
    Histogram aggregate;

    std::atomic<bool> done{false};
    std::thread writer([&recorder, &done] {
        for (int i = 0; i < kCount; ++i) {
            recorder.record(1'000 + i);
        }
        done.store(true);
    });

    while (!done.load(std::memory_order_relaxed)) {
        recorder.sample_into(aggregate);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    writer.join();
    recorder.sample_into(aggregate);  // final drain

    REQUIRE(aggregate.count() == kCount);
}
