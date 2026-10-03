// Microbenchmark for the swap pricer on a warm curve snapshot.

#include <alphaflow/core/date.hpp>
#include <alphaflow/curve/calendar.hpp>
#include <alphaflow/curve/curve_snapshot.hpp>
#include <alphaflow/curve/day_count.hpp>
#include <alphaflow/curve/schedule.hpp>
#include <alphaflow/pricing/swap_pricer.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>

namespace {

using alphaflow::core::Date;
using alphaflow::curve::Calendar;
using alphaflow::curve::CurveSnapshot;
using alphaflow::curve::DayCount;
using alphaflow::curve::Frequency;
using alphaflow::curve::ScheduleSpec;
using alphaflow::pricing::price_swap;
using alphaflow::pricing::SwapSpec;

CurveSnapshot make_snapshot() {
    CurveSnapshot snapshot;
    snapshot.generation = 1;
    snapshot.curve.reference = Date{2024, 1, 8};
    snapshot.curve.count = 6;
    for (std::size_t i = 0; i <= 5; ++i) {
        const Date d = Date{2024, 1, 8}.add_months(static_cast<int>(12 * i));
        const double t = snapshot.curve.time_of(d);
        snapshot.curve.times[i] = t;
        snapshot.curve.log_dfs[i] = -0.03 * t;
    }
    return snapshot;
}

SwapSpec make_swap() {
    ScheduleSpec spec;
    spec.effective = Date{2024, 1, 8};
    spec.termination = Date{2029, 1, 8};
    spec.frequency = Frequency::Annual;

    SwapSpec swap;
    swap.schedule = alphaflow::curve::make_schedule(spec, Calendar::united_states(),
                                                    DayCount::Actual360);
    swap.notional = 1'000'000.0;
    swap.fixed_rate = 0.03;
    return swap;
}

void BM_PriceSwap(benchmark::State& state) {
    const CurveSnapshot snapshot = make_snapshot();
    const SwapSpec swap = make_swap();
    for (auto _ : state) {
        benchmark::DoNotOptimize(price_swap(snapshot, swap));
    }
}
BENCHMARK(BM_PriceSwap);

}  // namespace
