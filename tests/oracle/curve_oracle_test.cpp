// QuantLib oracle tests (test-only; never linked into the engine).
//
// Two layers, per the agreed strategy:
//   * Layer 1: our log-linear discount curve matches QuantLib's
//     InterpolatedDiscountCurve<LogLinear> to machine precision, given the same
//     pillars.
//   * Layer 2: our OIS par rate matches QuantLib's OvernightIndexedSwap fair
//     rate on the *same* schedule and curve, to < 1e-9.

#include <alphaflow/core/date.hpp>
#include <alphaflow/curve/calendar.hpp>
#include <alphaflow/curve/day_count.hpp>
#include <alphaflow/curve/discount_curve.hpp>
#include <alphaflow/curve/ois_instrument.hpp>
#include <alphaflow/curve/schedule.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ql/handle.hpp>
#include <ql/shared_ptr.hpp>
#include <ql/indexes/ibor/sofr.hpp>
#include <ql/instruments/overnightindexedswap.hpp>
#include <ql/instruments/swap.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/yield/discountcurve.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/date.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/time/schedule.hpp>

#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace ql = QuantLib;

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

ql::Date to_ql(Date date) {
    return ql::Date(static_cast<ql::Day>(date.day()),
                    static_cast<ql::Month>(date.month()), date.year());
}

}  // namespace

TEST_CASE("our discount curve matches QuantLib's log-linear curve", "[oracle][curve]") {
    const std::vector<int> offsets = {0, 365, 730, 1096, 1461, 1826};
    const std::vector<double> dfs = {1.0, 0.98, 0.955, 0.928, 0.90, 0.87};

    DiscountCurve ours;
    ours.reference = Date{2020, 1, 1};
    ours.count = offsets.size();
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        ours.times[i] = static_cast<double>(offsets[i]) / 365.0;
        ours.log_dfs[i] = std::log(dfs[i]);
    }

    std::vector<ql::Date> dates;
    std::vector<ql::DiscountFactor> ql_dfs;
    for (const int offset : offsets) {
        dates.push_back(to_ql(Date{2020, 1, 1}.add_days(offset)));
    }
    for (const double df : dfs) {
        ql_dfs.push_back(df);
    }
    const auto reference =
        QuantLib::ext::make_shared<ql::InterpolatedDiscountCurve<ql::LogLinear>>(
            dates, ql_dfs, ql::Actual365Fixed());

    for (double t = 0.0; t <= 5.5; t += 0.1) {
        REQUIRE(ours.discount(t) == Catch::Approx(reference->discount(t, true)).epsilon(1e-12));
    }
}

TEST_CASE("our OIS par rate matches QuantLib's fair rate", "[oracle][ois]") {
    const Date reference_date{2020, 1, 1};
    constexpr double kRate = 0.03;
    constexpr int kYears = 6;

    // Evaluate as of the curve's reference so the swap projects from the curve
    // rather than demanding historical fixings.
    ql::Settings::instance().evaluationDate() = to_ql(reference_date);

    DiscountCurve ours;
    ours.reference = reference_date;
    ours.count = static_cast<std::size_t>(kYears) + 1;

    std::vector<ql::Date> curve_dates;
    std::vector<ql::DiscountFactor> curve_dfs;
    for (int i = 0; i <= kYears; ++i) {
        const Date d = reference_date.add_months(12 * i);
        const double t = static_cast<double>(d.serial() - reference_date.serial()) / 365.0;
        ours.times[static_cast<std::size_t>(i)] = t;
        ours.log_dfs[static_cast<std::size_t>(i)] = -kRate * t;
        curve_dates.push_back(to_ql(d));
        curve_dfs.push_back(std::exp(-kRate * t));
    }

    const auto ql_curve =
        QuantLib::ext::make_shared<ql::InterpolatedDiscountCurve<ql::LogLinear>>(
            curve_dates, curve_dfs, ql::Actual365Fixed());
    const ql::Handle<ql::YieldTermStructure> handle(ql_curve);

    const Calendar calendar = Calendar::united_states();
    ScheduleSpec spec;
    spec.effective = Date{2020, 1, 2};
    spec.termination = Date{2025, 1, 2};
    spec.frequency = Frequency::SemiAnnual;
    const Schedule schedule = make_schedule(spec, calendar, DayCount::Actual360);

    const double our_par = ois::par_rate(schedule, ours);
    REQUIRE(our_par > 0.0);

    // Feed QuantLib our exact adjusted period dates so the schedule is identical.
    std::vector<ql::Date> swap_dates;
    swap_dates.push_back(to_ql(schedule[0].start));
    for (std::size_t i = 0; i < schedule.count; ++i) {
        swap_dates.push_back(to_ql(schedule[i].end));
    }
    const ql::Schedule ql_schedule(swap_dates, ql::NullCalendar(), ql::Unadjusted);

    const auto index = QuantLib::ext::make_shared<ql::Sofr>(handle);
    ql::OvernightIndexedSwap swap(ql::FixedVsFloatingSwap::Receiver, 1.0e6, ql_schedule, 0.0,
                                  ql::Actual360(), index);
    swap.setPricingEngine(QuantLib::ext::make_shared<ql::DiscountingSwapEngine>(handle));
    const double ql_par = swap.fairRate();

    REQUIRE(our_par == Catch::Approx(ql_par).epsilon(1e-12));
}
