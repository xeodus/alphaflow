#include <alphaflow/metrics/histogram.hpp>

#include <hdr/hdr_histogram.h>

namespace alphaflow::metrics {
namespace {

hdr_histogram* allocate(Nanos lowest, Nanos highest, int significant_figures) noexcept {
    hdr_histogram* histogram = nullptr;
    if (hdr_init(lowest, highest, significant_figures, &histogram) != 0) {
        return nullptr;
    }
    return histogram;
}

}  // namespace

Histogram::Histogram() noexcept
    : Histogram(kDefaultLowest, kDefaultHighest, kDefaultSignificantFigures) {}

Histogram::Histogram(Nanos lowest, Nanos highest, int significant_figures) noexcept
    : histogram_(allocate(lowest, highest, significant_figures)) {}

Histogram::~Histogram() {
    if (histogram_ != nullptr) {
        hdr_close(histogram_);
    }
}

Histogram::Histogram(Histogram&& other) noexcept : histogram_(other.histogram_) {
    other.histogram_ = nullptr;
}

Histogram& Histogram::operator=(Histogram&& other) noexcept {
    if (this != &other) {
        if (histogram_ != nullptr) {
            hdr_close(histogram_);
        }
        histogram_ = other.histogram_;
        other.histogram_ = nullptr;
    }
    return *this;
}

bool Histogram::valid() const noexcept {
    return histogram_ != nullptr;
}

void Histogram::record(Nanos value) noexcept {
    if (histogram_ != nullptr) {
        // hdr_record_value returns false for out-of-range values, which the
        // 60 s default ceiling makes a non-issue in practice.
        static_cast<void>(hdr_record_value(histogram_, value));
    }
}

void Histogram::record_elapsed(Nanos start_ns, Nanos end_ns) noexcept {
    record(end_ns - start_ns);
}

std::uint64_t Histogram::count() const noexcept {
    return histogram_ != nullptr ? static_cast<std::uint64_t>(histogram_->total_count) : 0;
}

Nanos Histogram::min() const noexcept {
    return histogram_ != nullptr ? hdr_min(histogram_) : 0;
}

Nanos Histogram::max() const noexcept {
    return histogram_ != nullptr ? hdr_max(histogram_) : 0;
}

double Histogram::mean() const noexcept {
    return histogram_ != nullptr ? hdr_mean(histogram_) : 0.0;
}

Nanos Histogram::value_at_percentile(double percentile) const noexcept {
    if (histogram_ == nullptr) {
        return 0;
    }
    return static_cast<Nanos>(hdr_value_at_percentile(histogram_, percentile));
}

void Histogram::reset() noexcept {
    if (histogram_ != nullptr) {
        hdr_reset(histogram_);
    }
}

void Histogram::accumulate(const Histogram& other) noexcept {
    if (histogram_ != nullptr && other.histogram_ != nullptr) {
        static_cast<void>(hdr_add(histogram_, other.histogram_));
    }
}

LatencyProfile Histogram::profile(Stage stage) const noexcept {
    LatencyProfile profile;
    profile.stage = stage;
    profile.count = count();
    if (profile.count == 0 || histogram_ == nullptr) {
        return profile;
    }
    profile.min_ns = min();
    profile.max_ns = max();
    profile.mean_ns = static_cast<Nanos>(mean());
    profile.p50_ns = value_at_percentile(50.0);
    profile.p90_ns = value_at_percentile(90.0);
    profile.p99_ns = value_at_percentile(99.0);
    profile.p999_ns = value_at_percentile(99.9);
    return profile;
}

LatencyRecorder::LatencyRecorder(Stage stage, Nanos lowest, Nanos highest,
                                 int significant_figures) noexcept
    : stage_(stage), histogram_(lowest, highest, significant_figures) {}

}  // namespace alphaflow::metrics
