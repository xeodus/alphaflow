#include <alphaflow/metrics/interval_recorder.hpp>

#include <alphaflow/metrics/histogram.hpp>

#include <hdr/hdr_histogram.h>
#include <hdr/hdr_interval_recorder.h>

#include <new>

namespace alphaflow::metrics {

IntervalRecorder::IntervalRecorder(Nanos lowest, Nanos highest,
                                   int significant_figures) noexcept {
    auto* recorder = new (std::nothrow) hdr_interval_recorder();
    if (recorder == nullptr) {
        return;
    }
    if (hdr_interval_recorder_init_all(recorder, lowest, highest, significant_figures) != 0) {
        delete recorder;
        return;
    }

    hdr_histogram* recycle = nullptr;
    if (hdr_init(lowest, highest, significant_figures, &recycle) != 0) {
        hdr_interval_recorder_destroy(recorder);
        delete recorder;
        return;
    }

    recorder_ = recorder;
    recycle_ = recycle;
}

IntervalRecorder::~IntervalRecorder() {
    if (recycle_ != nullptr) {
        hdr_close(recycle_);
    }
    if (recorder_ != nullptr) {
        hdr_interval_recorder_destroy(recorder_);
        delete recorder_;
    }
}

bool IntervalRecorder::valid() const noexcept {
    return recorder_ != nullptr;
}

void IntervalRecorder::record(Nanos value) noexcept {
    if (recorder_ != nullptr) {
        static_cast<void>(hdr_interval_recorder_record_value(recorder_, value));
    }
}

void IntervalRecorder::record_elapsed(Nanos start_ns, Nanos end_ns) noexcept {
    record(end_ns - start_ns);
}

void IntervalRecorder::sample_into(Histogram& target) noexcept {
    if (recorder_ == nullptr || recycle_ == nullptr || !target.valid()) {
        return;
    }

    hdr_histogram* interval = hdr_interval_recorder_sample_and_recycle(recorder_, recycle_);
    if (interval == nullptr) {
        return;
    }

    static_cast<void>(hdr_add(target.handle(), interval));
    hdr_reset(interval);
    recycle_ = interval;
}

}  // namespace alphaflow::metrics
