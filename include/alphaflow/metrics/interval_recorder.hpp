#pragma once

#include <alphaflow/platform/clock.hpp>

#include <cstdint>

struct hdr_interval_recorder;
struct hdr_histogram;

namespace alphaflow::metrics {

class Histogram;

/// Lock-free interval recorder for the live Metrics thread (ARCHITECTURE.md
/// §11, ADR-012).
///
/// Recording threads call `record` without blocking. A single sampling thread
/// periodically calls `sample_into(aggregate)` to move the interval's samples
/// into a long-lived aggregate and recycle the buffer. The recorder's internal
/// writer/reader phaser coordinates the two sides, so recording and sampling
/// are safe concurrently.
class IntervalRecorder {
public:
    explicit IntervalRecorder(platform::Nanos lowest = 1,
                              platform::Nanos highest = 60'000'000'000LL,
                              int significant_figures = 3) noexcept;
    ~IntervalRecorder();

    IntervalRecorder(const IntervalRecorder&) = delete;
    IntervalRecorder& operator=(const IntervalRecorder&) = delete;
    IntervalRecorder(IntervalRecorder&&) = delete;
    IntervalRecorder& operator=(IntervalRecorder&&) = delete;

    [[nodiscard]] bool valid() const noexcept;

    /// Recording-thread side.
    void record(platform::Nanos value) noexcept;
    void record_elapsed(platform::Nanos start_ns, platform::Nanos end_ns) noexcept;

    /// Sampling-thread side. Moves the interval's samples into `target`.
    void sample_into(Histogram& target) noexcept;

private:
    hdr_interval_recorder* recorder_{nullptr};
    hdr_histogram* recycle_{nullptr};
};

}  // namespace alphaflow::metrics
