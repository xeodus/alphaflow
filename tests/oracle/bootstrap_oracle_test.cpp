// Oracle: our bootstrapped curve vs QuantLib's own OIS bootstrap (criterion 3).
//
// QuantLib's OISRateHelper generates its own schedule and fixed-leg day count,
// so this test configures it to match ours (0 settlement days, annual fixed,
// ACT/360, ModifiedFollowing, US Government Bond calendar, forward generation)
// and compares discount factors. A round-trip check (QuantLib repricing our
// bootstrapped curve to par on our exact schedules) backs it up.

#include <alphaflow/core/date.hpp>
#include <alphaflow/curve/bootstrap.hpp>
#include <alphaflow/curve/calendar.hpp>
#include <alphaflow/curve/day_count.hpp>
#include <alphaflow/curve/discount_curve.hpp>
#include <alphaflow/curve/ois_instrument.hpp>
#include <alphaflow/curve/schedule.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ql/handle.hpp>
#include <ql/indexes/ibor/sofr.hpp>
#include <ql/instruments/overnightindexedswap.hpp>
#include <ql/instruments/swap.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/settings.hpp>
#include <ql/shared_ptr.hpp>
#include <ql/termstructures/yield/bootstraptraits.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/time/calendars/unitedstates.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/time/dategenerationrule.hpp>
#include <ql/time/period.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace ql = QuantLib;

namespace {

using alphaflow::core::Date;
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
const std::array<double, kPillars> kQuotes = {0.025, 0.030, 0.033, 0.036, 0.038};

ql::Date to_ql(Date date) {
    return ql::Date(static_cast<ql::Day>(date.day()),
                    static_cast<ql::Month>(date.month()), date.year());
}

Date maturity(std::size_t index) {
    return kSpot.add_months(static_cast<int>(12 * (index + 1)));
}

}  // namespace

TEST_CASE("our bootstrap matches QuantLib's OIS bootstrap", "[oracle][bootstrap]") {
    // --- ours ---
    std::array<OisPillar, kPillars> pillars{};
    for (std::size_t i = 0; i < kPillars; ++i) {
        pillars[i] = OisPillar{maturity(i), kQuotes[i]};
    }
    BootstrapSpec spec;
    spec.reference = kSpot;
    spec.frequency = Frequency::Annual;
    const DiscountCurve ours =
        alphaflow::curve::bootstrap(pillars, Calendar::united_states(), spec).curve;

    // --- QuantLib ---
    ql::Settings::instance().evaluationDate() = to_ql(kSpot);
    const ql::Calendar us = ql::UnitedStates(ql::UnitedStates::GovernmentBond);
    const auto index = QuantLib::ext::make_shared<ql::Sofr>();

    std::vector<QuantLib::ext::shared_ptr<ql::RateHelper>> helpers;
    helpers.reserve(kPillars);
    for (std::size_t i = 0; i < kPillars; ++i) {
        helpers.push_back(QuantLib::ext::make_shared<ql::OISRateHelper>(
            0, ql::Period(static_cast<int>(i + 1), ql::Years), kQuotes[i], index,
            ql::Handle<ql::YieldTermStructure>(), false, 0, ql::ModifiedFollowing, ql::Annual,
            us, ql::Period(0, ql::Days), 0.0, ql::Pillar::LastRelevantDate, ql::Date(),
            ql::RateAveraging::Compound, QuantLib::ext::nullopt, ql::Annual, us,
            ql::Null<ql::Natural>(), 0, false,
            QuantLib::ext::shared_ptr<ql::FloatingRateCouponPricer>(),
            ql::DateGeneration::Forward, us, ql::ModifiedFollowing));
    }
    const auto curve =
        QuantLib::ext::make_shared<ql::PiecewiseYieldCurve<ql::Discount, ql::LogLinear>>(
            to_ql(kSpot), helpers, ql::Actual365Fixed());

    // Compare discount factors at each pillar.
    for (std::size_t i = 0; i < kPillars; ++i) {
        const double t = ours.time_of(maturity(i));
        REQUIRE(ours.discount(t) ==
                Catch::Approx(curve->discount(to_ql(maturity(i)), true)).epsilon(1e-9));
    }
}

TEST_CASE("QuantLib reprices our bootstrapped curve to par", "[oracle][bootstrap]") {
    std::array<OisPillar, kPillars> pillars{};
    for (std::size_t i = 0; i < kPillars; ++i) {
        pillars[i] = OisPillar{maturity(i), kQuotes[i]};
    }
    BootstrapSpec spec;
    spec.reference = kSpot;
    spec.frequency = Frequency::Annual;
    const DiscountCurve ours =
        alphaflow::curve::bootstrap(pillars, Calendar::united_states(), spec).curve;

    // Build a QuantLib curve from our exact discount factors.
    std::vector<ql::Date> dates;
    std::vector<ql::DiscountFactor> dfs;
    for (std::size_t i = 0; i < ours.count; ++i) {
        const Date d = kSpot.add_days(static_cast<int>(ours.times[i] * 365.0 + 0.5));
        dates.push_back(to_ql(d));
        dfs.push_back(ours.discount(ours.times[i]));
    }
    const auto curve = QuantLib::ext::make_shared<ql::InterpolatedDiscountCurve<ql::LogLinear>>(
        dates, dfs, ql::Actual365Fixed());
    const ql::Handle<ql::YieldTermStructure> handle(curve);

    // Evaluate the day before the first accrual so overnight fixings project
    // from the curve instead of being looked up historically.
    ql::Settings::instance().evaluationDate() = to_ql(kSpot) - 1;

    const auto index = QuantLib::ext::make_shared<ql::Sofr>(handle);
    for (std::size_t i = 0; i < kPillars; ++i) {
        ScheduleSpec schedule_spec;
        schedule_spec.effective = kSpot;
        schedule_spec.termination = maturity(i);
        schedule_spec.frequency = Frequency::Annual;
        const Schedule schedule =
            make_schedule(schedule_spec, Calendar::united_states(), DayCount::Actual360);

        std::vector<ql::Date> swap_dates;
        swap_dates.push_back(to_ql(schedule[0].start));
        for (std::size_t j = 0; j < schedule.count; ++j) {
            swap_dates.push_back(to_ql(schedule[j].end));
        }
        const ql::Schedule ql_schedule(swap_dates, ql::NullCalendar(), ql::Unadjusted);

        ql::OvernightIndexedSwap swap(ql::FixedVsFloatingSwap::Receiver, 1.0e6, ql_schedule,
                                      kQuotes[i], ql::Actual360(), index);
        swap.setPricingEngine(QuantLib::ext::make_shared<ql::DiscountingSwapEngine>(handle));
        REQUIRE(swap.NPV() == Catch::Approx(0.0).margin(1.0));  // < $1 on $1m
    }
}
