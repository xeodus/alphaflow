// Unit tests for alphaflow::curve::bootstrap.
//
// The bootstrap is validated by self-consistency: quotes generated from a known
// curve must be recovered exactly, and every bootstrapped pillar must reprice to
// par. The independent QuantLib comparison is added in the oracle target.

#include <alphaflow/curve/bootstrap.hpp>

#include <alphaflow/core/date.hpp>
#include <alphaflow/curve/calendar.hpp>
#include <alphaflow/curve/day_count.hpp>
#include <alphaflow/curve/ois_instrument.hpp>
#include <alphaflow/curve/schedule.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

namespace {

using alphaflow::core::Date;
using alphaflow::curve::bootstrap;
using alphaflow::curve::BootstrapResult;
using alphaflow::curve::BootstrapSpec;
using alphaflow::curve::Calendar;
using alphaflow::curve::DayCount;
using alphaflow::curve::DiscountCurve;
using alphaflow::curve::Frequency;
using alphaflow::curve::make_schedule;
using alphaflow::curve::OisPillar;
using alphaflow::curve::Schedule;
using alphaflow::curve::ScheduleSpec;
namespace ois = alphaflow::curve::ois;

constexpr std::size_t kPillars = 5;
const Date kSpot{2024, 1, 8};

Date maturity(std::size_t index) {
    return kSpot.add_months(static_cast<int>(12 * (index + 1)));
}

std::array<Schedule, kPillars> build_schedules(const Calendar& calendar) {
    std::array<Schedule, kPillars> schedules{};
    for (std::size_t i = 0; i < kPillars; ++i) {
        ScheduleSpec spec;
        spec.effective = kSpot;
        spec.termination = maturity(i);
        spec.frequency = Frequency::Annual;
        schedules[i] = make_schedule(spec, calendar, DayCount::Actual360);
    }
    return schedules;
}

}  // namespace

TEST_CASE("bootstrap recovers a known curve from its par rates",
          "[curve][bootstrap]") {
    const Calendar calendar = Calendar::united_states();
    const auto schedules = build_schedules(calendar);
    constexpr double kRate = 0.03;

    // A known log-linear curve with pillars at each schedule's last payment.
    DiscountCurve known;
    known.reference = kSpot;
    known.count = kPillars + 1;
    known.times[0] = 0.0;
    known.log_dfs[0] = 0.0;
    for (std::size_t i = 0; i < kPillars; ++i) {
        const Schedule& schedule = schedules[i];
        const double t = known.time_of(schedule.periods[schedule.count - 1].end);
        known.times[i + 1] = t;
        known.log_dfs[i + 1] = -kRate * t;
    }

    std::array<OisPillar, kPillars> pillars{};
    for (std::size_t i = 0; i < kPillars; ++i) {
        pillars[i] = OisPillar{maturity(i), ois::par_rate(schedules[i], known)};
    }

    BootstrapSpec spec;
    spec.reference = kSpot;
    spec.frequency = Frequency::Annual;
    const BootstrapResult result = bootstrap(pillars, calendar, spec);

    REQUIRE(result.converged);
    REQUIRE(result.pillars == kPillars);
    for (std::size_t i = 0; i <= kPillars; ++i) {
        REQUIRE(result.curve.log_dfs[i] == Catch::Approx(known.log_dfs[i]).epsilon(1e-10));
    }
}

TEST_CASE("each bootstrapped pillar reprices to par", "[curve][bootstrap]") {
    const Calendar calendar = Calendar::united_states();
    const auto schedules = build_schedules(calendar);

    const std::array<double, kPillars> quotes = {0.020, 0.025, 0.030, 0.035, 0.040};
    std::array<OisPillar, kPillars> pillars{};
    for (std::size_t i = 0; i < kPillars; ++i) {
        pillars[i] = OisPillar{maturity(i), quotes[i]};
    }

    BootstrapSpec spec;
    spec.reference = kSpot;
    spec.frequency = Frequency::Annual;
    const BootstrapResult result = bootstrap(pillars, calendar, spec);
    REQUIRE(result.converged);

    for (std::size_t i = 0; i < kPillars; ++i) {
        REQUIRE(ois::par_rate(schedules[i], result.curve) ==
                Catch::Approx(quotes[i]).epsilon(1e-10));
    }
}

TEST_CASE("bootstrapped discount factors are positive and decreasing",
          "[curve][bootstrap]") {
    const Calendar calendar = Calendar::united_states();

    std::array<OisPillar, kPillars> pillars{};
    for (std::size_t i = 0; i < kPillars; ++i) {
        pillars[i] = OisPillar{maturity(i), 0.02 + 0.005 * static_cast<double>(i)};
    }

    BootstrapSpec spec;
    spec.reference = kSpot;
    spec.frequency = Frequency::Annual;
    const BootstrapResult result = bootstrap(pillars, calendar, spec);

    for (std::size_t i = 0; i <= kPillars; ++i) {
        REQUIRE(result.curve.discount(result.curve.times[i]) > 0.0);
        if (i > 0) {
            REQUIRE(result.curve.discount(result.curve.times[i]) <
                    result.curve.discount(result.curve.times[i - 1]));
        }
    }
}

TEST_CASE("bootstrapping nothing yields the flat reference pillar",
          "[curve][bootstrap]") {
    const Calendar calendar = Calendar::united_states();
    BootstrapSpec spec;
    spec.reference = kSpot;

    const BootstrapResult result = bootstrap({}, calendar, spec);
    REQUIRE(result.converged);
    REQUIRE(result.pillars == 0);
    REQUIRE(result.curve.count == 1);
    REQUIRE(result.curve.discount(0.0) == Catch::Approx(1.0));
}
