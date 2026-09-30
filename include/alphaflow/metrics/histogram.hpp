#pragma once

#include <alphaflow/platform/clock.hpp>

#include <cstdint>
#include <string_view>

// The HdrHistogram C type is kept opaque here so the C API does not leak into
// engine code.
struct hdr_histogram;

namespace alphaflow::metrics {

using Nanos = platform::Nanos;

/// A measured stage. Every reported latency belongs to one of these.
enum class Stage : std::uint8_t {
    Ingest,
    Rebuild,
    Rfq,
};

[[nodiscard]] constexpr std::string_view stage_name(Stage stage) noexcept {
    switch (stage) {
        case Stage::Ingest:
            return "ingest";
        case Stage::Rebuild:
            return "rebuild";
        case Stage::Rfq:
            return "rfq";
    }
    return "unknown";
}

/// The exact interval a stage measures. This is the contract from
/// ARCHITECTURE.md §11 (ADR-012): a latency number is meaningless without the
/// span it was taken over, so the span is written down next to the number and
/// pinned by a test. In particular the RFQ span is socket-to-socket, not the
/// smaller in-process handler time.
[[nodiscard]] constexpr std::string_view stage_span(Stage stage) noexcept {
    switch (stage) {
        case Stage::Ingest:
            return "feed arrival -> tick timestamped";
        case Stage::Rebuild:
            return "first changed pillar -> curve snapshot published";
        case Stage::Rfq:
            return "request bytes read off socket -> response bytes handed to socket";
    }
    return "unknown";
}

struct LatencyProfile {
    Stage stage{Stage::Rfq};
    std::uint64_t count{};
    Nanos min_ns{};
    Nanos max_ns{};
    Nanos mean_ns{};
    Nanos p50_ns{};
    Nanos p90_ns{};
    Nanos p99_ns{};
    Nanos p999_ns{};
};

/// A single-threaded HDR histogram over nanoseconds.
///
/// Owned by exactly one thread so that recording is uncontended and needs no
/// synchronisation. It must not be read by another thread while it is being
/// recorded into; aggregate after the recording thread has stopped, or hand
/// off a copy at a quiescent point.
///
/// Resolution: HdrHistogram reports the *highest equivalent value* of a
/// bucket, so `max()` and the upper percentiles are upper bounds within one
/// part in 10^significant_figures of the true value (~0.1% at the default of 3
/// significant figures). The reported tail is therefore never an under-estimate.
class Histogram {
public:
    // Defaults: 1 ns up to 60 s, 3 significant figures (~0.1% accuracy).
    static constexpr Nanos kDefaultLowest = 1;
    static constexpr Nanos kDefaultHighest = 60'000'000'000LL;
    static constexpr int kDefaultSignificantFigures = 3;

    Histogram() noexcept;
    explicit Histogram(Nanos lowest, Nanos highest, int significant_figures) noexcept;
    ~Histogram();

    Histogram(const Histogram&) = delete;
    Histogram& operator=(const Histogram&) = delete;
    Histogram(Histogram&& other) noexcept;
    Histogram& operator=(Histogram&& other) noexcept;

    /// False if the backing histogram could not be allocated.
    [[nodiscard]] bool valid() const noexcept;

    void record(Nanos value) noexcept;
    void record_elapsed(Nanos start_ns, Nanos end_ns) noexcept;

    [[nodiscard]] std::uint64_t count() const noexcept;
    [[nodiscard]] Nanos min() const noexcept;
    [[nodiscard]] Nanos max() const noexcept;
    [[nodiscard]] double mean() const noexcept;
    [[nodiscard]] Nanos value_at_percentile(double percentile) const noexcept;

    void reset() noexcept;

    /// Merge `other` into this histogram. `other` is left unchanged.
    void accumulate(const Histogram& other) noexcept;

    [[nodiscard]] LatencyProfile profile(Stage stage) const noexcept;

private:
    hdr_histogram* histogram_{nullptr};
};

/// Per-thread, single-stage recorder. Binds a stage (for its name and span) to
/// a histogram, so a reported profile always carries the interval it measured.
class LatencyRecorder {
public:
    explicit LatencyRecorder(Stage stage,
                             Nanos lowest = Histogram::kDefaultLowest,
                             Nanos highest = Histogram::kDefaultHighest,
                             int significant_figures = Histogram::kDefaultSignificantFigures) noexcept;

    void record(Nanos value) noexcept { histogram_.record(value); }
    void record_elapsed(Nanos start_ns, Nanos end_ns) noexcept {
        histogram_.record_elapsed(start_ns, end_ns);
    }
    void reset() noexcept { histogram_.reset(); }

    [[nodiscard]] bool valid() const noexcept { return histogram_.valid(); }
    [[nodiscard]] const Histogram& histogram() const noexcept { return histogram_; }
    [[nodiscard]] LatencyProfile profile() const noexcept { return histogram_.profile(stage_); }

private:
    Stage stage_;
    Histogram histogram_;
};

}  // namespace alphaflow::metrics
