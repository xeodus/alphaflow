// Unit tests for alphaflow::curve::ois swap math.
//
// These pin the two identities the architecture makes into tests
// (ARCHITECTURE.md §7.5): the compounded-OIS floating leg telescopes to
// N·(D(t0) − D(tn)), and the par rate is (D(t0) − D(tn)) / Σ δ_j D(t_j), with
// PV = 0 at par. There is no hardcoded DV01 here; sensitivity is bump-and-
// revalue, later.

#include <alphaflow/curve/ois_instrument.hpp>

#include <alphaflow/core/date.hpp>
#include <alphaflow/curve/calendar.hpp>
#include <alphaflow/curve/day_count.hpp>
#include <alphaflow/curve/discount_curve.hpp>
#include <alphaflow/curve/schedule.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>

namespace {

using alphaflow::core::Date;
using alphaflow::curve::Calendar;
using alphaflow::curve::DayCount;
using alphaflow::curve::DiscountCurve;
using alphaflow::curve::Frequency;
using alphaflow::curve::make_schedule;
using alphaflow::curve::Schedule;
using alphaflow::curve::ScheduleSpec;
namespace ois = alphaflow::curve::ois;

/// A flat continuously-compounded curve D(t) = exp(-rate * t) with pillars at
/// integer years, so interpolation is exact everywhere in range.
DiscountCurve flat_curve(double rate, std::size_t years) {
    DiscountCurve curve;
    curve.reference = Date{2023, 1, 1};
    curve.count = years + 1;
    for (std::size_t i = 0; i <= years; ++i) {
        const double t = static_cast<double>(i);
        curve.times[i] = t;
        curve.log_dfs[i] = -rate * t;
    }
    return curve;
}

}  // namespace

TEST_CASE("a single-period swap is hand-computable", "[curve][ois]") {
    // 2023-01-01 -> 2024-01-01 is exactly 365 days, so t maps to 0 and 1.
    DiscountCurve curve;
    curve.reference = Date{2023, 1, 1};
    curve.count = 2;
    curve.times = {0.0, 1.0};
    curve.log_dfs = {0.0, -0.1};

    Schedule schedule;
    schedule.count = 1;
    schedule.day_count = DayCount::Actual360;
    schedule.periods[0].start = Date{2023, 1, 1};
    schedule.periods[0].end = Date{2024, 1, 1};
    schedule.periods[0].payment = Date{2024, 1, 1};
    schedule.periods[0].accrual = 365.0 / 360.0;

    const double expected_annuity = (365.0 / 360.0) * std::exp(-0.1);
    const double expected_par = (1.0 - std::exp(-0.1)) / expected_annuity;

    REQUIRE(ois::annuity(schedule, curve) == Catch::Approx(expected_annuity).epsilon(1e-12));
    REQUIRE(ois::floating_leg_pv(schedule, curve, 1.0) ==
            Catch::Approx(1.0 - std::exp(-0.1)).epsilon(1e-12));
    REQUIRE(ois::par_rate(schedule, curve) == Catch::Approx(expected_par).epsilon(1e-12));
}

TEST_CASE("a swap prices to zero at its par rate", "[curve][ois]") {
    const DiscountCurve curve = flat_curve(0.05, 3);
    const Calendar calendar = Calendar::united_states();
    const Schedule schedule =
        make_schedule(ScheduleSpec{Date{2023, 1, 2}, Date{2025, 1, 2}}, calendar,
                      DayCount::Actual360);

    const double par = ois::par_rate(schedule, curve);
    REQUIRE(ois::pv_receive_fixed(schedule, curve, par, 1'000'000.0) ==
            Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("par rate equals the definition from the curve", "[curve][ois]") {
    const DiscountCurve curve = flat_curve(0.05, 3);
    const Calendar calendar = Calendar::united_states();
    const Schedule schedule =
        make_schedule(ScheduleSpec{Date{2023, 1, 2}, Date{2025, 1, 2}}, calendar,
                      DayCount::Actual360);

    double expected_fixed = 0.0;
    for (std::size_t i = 0; i < schedule.count; ++i) {
        expected_fixed += schedule.periods[i].accrual * curve.discount(schedule.periods[i].payment);
    }
    const double expected_float =
        curve.discount(schedule.periods[0].start) -
        curve.discount(schedule.periods[schedule.count - 1].end);

    REQUIRE(ois::annuity(schedule, curve) == Catch::Approx(expected_fixed).epsilon(1e-12));
    REQUIRE(ois::floating_leg_pv(schedule, curve, 1.0) ==
            Catch::Approx(expected_float).epsilon(1e-12));
    REQUIRE(ois::par_rate(schedule, curve) ==
            Catch::Approx(expected_float / expected_fixed).epsilon(1e-12));
}

TEST_CASE("the floating leg ignores coupon frequency", "[curve][ois]") {
    const DiscountCurve curve = flat_curve(0.05, 3);
    const Calendar calendar = Calendar::united_states();

    ScheduleSpec semi;
    semi.effective = Date{2023, 1, 2};
    semi.termination = Date{2025, 1, 2};
    semi.frequency = Frequency::SemiAnnual;

    ScheduleSpec quarterly = semi;
    quarterly.frequency = Frequency::Quarterly;

    const Schedule semi_schedule = make_schedule(semi, calendar, DayCount::Actual360);
    const Schedule quarterly_schedule = make_schedule(quarterly, calendar, DayCount::Actual360);

    REQUIRE(ois::floating_leg_pv(semi_schedule, curve, 1.0) ==
            Catch::Approx(ois::floating_leg_pv(quarterly_schedule, curve, 1.0)).epsilon(1e-12));
}

TEST_CASE("a swap scales with notional and the par rate does not", "[curve][ois]") {
    const DiscountCurve curve = flat_curve(0.05, 3);
    const Calendar calendar = Calendar::united_states();
    const Schedule schedule =
        make_schedule(ScheduleSpec{Date{2023, 1, 2}, Date{2025, 1, 2}}, calendar,
                      DayCount::Actual360);

    const double par = ois::par_rate(schedule, curve);
    const double one = ois::pv_receive_fixed(schedule, curve, par + 0.001, 1'000'000.0);
    const double two = ois::pv_receive_fixed(schedule, curve, par + 0.001, 2'000'000.0);
    REQUIRE(two == Catch::Approx(2.0 * one).epsilon(1e-12));
}

TEST_CASE("a receiver gains when the fixed rate is above par", "[curve][ois]") {
    const DiscountCurve curve = flat_curve(0.05, 3);
    const Calendar calendar = Calendar::united_states();
    const Schedule schedule =
        make_schedule(ScheduleSpec{Date{2023, 1, 2}, Date{2025, 1, 2}}, calendar,
                      DayCount::Actual360);

    const double par = ois::par_rate(schedule, curve);
    REQUIRE(ois::pv_receive_fixed(schedule, curve, par + 0.001, 1'000'000.0) > 0.0);
    REQUIRE(ois::pv_receive_fixed(schedule, curve, par - 0.001, 1'000'000.0) < 0.0);
}

TEST_CASE("the fixed leg is notional times rate times the annuity", "[curve][ois]") {
    const DiscountCurve curve = flat_curve(0.05, 3);
    const Calendar calendar = Calendar::united_states();
    const Schedule schedule =
        make_schedule(ScheduleSpec{Date{2023, 1, 2}, Date{2025, 1, 2}}, calendar,
                      DayCount::Actual360);

    REQUIRE(ois::fixed_leg_pv(schedule, curve, 0.04, 5'000'000.0) ==
            Catch::Approx(5'000'000.0 * 0.04 * ois::annuity(schedule, curve)).epsilon(1e-12));
}

TEST_CASE("an empty schedule yields zero", "[curve][ois]") {
    const DiscountCurve curve = flat_curve(0.05, 3);
    const Schedule empty;

    REQUIRE(ois::annuity(empty, curve) == 0.0);
    REQUIRE(ois::floating_leg_pv(empty, curve, 1'000'000.0) == 0.0);
    REQUIRE(ois::par_rate(empty, curve) == 0.0);
    REQUIRE(ois::pv_receive_fixed(empty, curve, 0.04, 1'000'000.0) == 0.0);
}
