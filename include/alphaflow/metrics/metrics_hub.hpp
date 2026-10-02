#pragma once

#include <alphaflow/metrics/histogram.hpp>
#include <alphaflow/metrics/interval_recorder.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

namespace alphaflow::metrics {

/// The live Metrics thread's registry (ARCHITECTURE.md §11).
///
/// Workers record into their own `IntervalRecorder`s; this hub is the single
/// sampling thread. It periodically harvests every registered recorder into a
/// per-stage aggregate, so the histogram is the source of truth without the
/// recording threads ever blocking.
class MetricsHub {
public:
    static constexpr std::size_t kMaxStages = 4;

    /// Register a recorder for a stage. Returns false if at capacity.
    [[nodiscard]] bool add(Stage stage, IntervalRecorder& recorder) noexcept {
        if (count_ >= kMaxStages) {
            return false;
        }
        entries_[count_] = Entry{stage, &recorder};
        ++count_;
        return true;
    }

    /// Harvest every registered recorder once.
    void sample_once() noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            entries_[i].recorder->sample_into(aggregates_[i]);
        }
    }

    /// Thread body: sample every `interval` until `stop`, then a final sample.
    void run(const std::atomic<bool>& stop, std::chrono::milliseconds interval) noexcept {
        while (!stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(interval);
            sample_once();
        }
        sample_once();
    }

    [[nodiscard]] LatencyProfile profile(Stage stage) const noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            if (entries_[i].stage == stage) {
                return aggregates_[i].profile(stage);
            }
        }
        return LatencyProfile{stage};
    }

private:
    struct Entry {
        Stage stage{Stage::Ingest};
        IntervalRecorder* recorder{nullptr};
    };

    std::array<Entry, kMaxStages> entries_{};
    std::array<Histogram, kMaxStages> aggregates_{};
    std::size_t count_{0};
};

}  // namespace alphaflow::metrics
