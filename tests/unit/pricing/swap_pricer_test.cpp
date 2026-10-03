// Unit tests for alphaflow::pricing::price_swap / dv01.

#include <alphaflow/pricing/swap_pricer.hpp>

#include <alphaflow/core/date.hpp>
#include <alphaflow/curve/calendar.hpp>
#include <alphaflow/curve/day_count.hpp>
#include <alphaflow/curve/schedule.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>

namespace {

using alphaflow::core::Date;
using alphaflow::curve::Calendar;
using alphaflow::curve::CurveSnapshot;
using alphaflow::curve::DayCount;
using alphaflow::curve::Frequency;
using alphaflow::curve::make_schedule;
using alphaflow::curve::ScheduleSpec;
using alphaflow::pricing::dv01;
using alphaflow::pricing::price_swap;
using alphaflow::pricing::SwapSpec;

CurveSnapshot flat_snapshot(double rate) {
    CurveSnapshot snapshot;
    snapshot.generation = 1;
    snapshot.curve.reference = Date{2024, 1, 8};
    snapshot.curve.count = 6;
    for (std::size_t i = 0; i <= 5; ++i) {
        const Date d = Date{2024, 1, 8}.add_months(static_cast<int>(12 * i));
        const double t = snapshot.curve.time_of(d);
        snapshot.curve.times[i] = t;
        snapshot.curve.log_dfs[i] = -rate * t;
    }
    return snapshot;
}

SwapSpec five_year_swap(double fixed_rate, double notional) {
    ScheduleSpec spec;
    spec.effective = Date{2024, 1, 8};
    spec.termination = Date{2029, 1, 8};
    spec.frequency = Frequency::Annual;

    SwapSpec swap;
    swap.schedule = make_schedule(spec, Calendar::united_states(), DayCount::Actual360);
    swap.fixed_rate = fixed_rate;
    swap.notional = notional;
    return swap;
}

}  // namespace

TEST_CASE("a swap prices to zero at its par rate", "[pricing][swap]") {
    const CurveSnapshot snapshot = flat_snapshot(0.03);
    SwapSpec spec = five_year_swap(0.0, 1'000'000.0);

    const auto at_zero = price_swap(snapshot, spec);
    spec.fixed_rate = at_zero.par_rate;

    const auto at_par = price_swap(snapshot, spec);
    REQUIRE(at_par.par_rate > 0.0);
    REQUIRE(at_par.pv == Catch::Approx(0.0).margin(1e-6));
}

TEST_CASE("DV01 is signed, plausible, and scales with notional", "[pricing][swap]") {
    const CurveSnapshot snapshot = flat_snapshot(0.03);
    SwapSpec spec = five_year_swap(0.03, 1'000'000.0);

    const double receiver_dv01 = dv01(snapshot, spec);
    REQUIRE(receiver_dv01 < 0.0);               // a receiver loses when rates rise
    REQUIRE(std::abs(receiver_dv01) > 100.0);   // > $100 per bp on $1m 5Y
    REQUIRE(std::abs(receiver_dv01) < 2'000.0);

    SwapSpec doubled = spec;
    doubled.notional = 2'000'000.0;
    REQUIRE(dv01(snapshot, doubled) == Catch::Approx(2.0 * receiver_dv01).epsilon(1e-9));
}
