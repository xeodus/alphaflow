// Unit tests for alphaflow::curve::make_schedule.
//
// Schedules decide every accrual and payment date, so they are tested against
// hand-computed dates: T+2 spot, a regular grid, a short back stub, month-end
// preservation, business-day-adjusted payments, and a short front stub.

#include <alphaflow/curve/schedule.hpp>

#include <alphaflow/core/date.hpp>
#include <alphaflow/curve/calendar.hpp>
#include <alphaflow/curve/day_count.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

using alphaflow::core::Date;
using alphaflow::curve::Calendar;
using alphaflow::curve::DayCount;
using alphaflow::curve::Frequency;
using alphaflow::curve::make_schedule;
using alphaflow::curve::Schedule;
using alphaflow::curve::ScheduleSpec;
using alphaflow::curve::spot_date;
using alphaflow::curve::StubPolicy;

ScheduleSpec spec(Date effective, Date termination, Frequency frequency = Frequency::SemiAnnual) {
    ScheduleSpec result;
    result.effective = effective;
    result.termination = termination;
    result.frequency = frequency;
    return result;
}

}  // namespace

TEST_CASE("spot is T+2 business days", "[curve][schedule]") {
    const Calendar calendar = Calendar::united_states();

    REQUIRE(spot_date(Date{2024, 1, 4}, calendar) == Date{2024, 1, 8});   // Thu -> Mon
    REQUIRE(spot_date(Date{2024, 1, 8}, calendar) == Date{2024, 1, 10});  // Mon -> Wed
}

TEST_CASE("a 5Y semi-annual schedule is regular and lands on the termination",
          "[curve][schedule]") {
    const Calendar calendar = Calendar::united_states();

    const Schedule schedule = make_schedule(spec(Date{2024, 1, 8}, Date{2029, 1, 8}), calendar,
                                            DayCount::Actual360);

    REQUIRE(schedule.count == 10);
    REQUIRE(schedule[0].start == Date{2024, 1, 8});
    REQUIRE(schedule[0].end == Date{2024, 7, 8});
    REQUIRE(schedule.payment(9) == Date{2029, 1, 8});
}

TEST_CASE("a short back stub is the final period", "[curve][schedule]") {
    const Calendar calendar = Calendar::united_states();

    const Schedule schedule =
        make_schedule(spec(Date{2024, 1, 8}, Date{2026, 10, 8}), calendar, DayCount::Actual360);

    REQUIRE(schedule.count == 6);
    REQUIRE(schedule[5].end == Date{2026, 10, 8});
    REQUIRE(schedule[5].accrual < schedule[0].accrual);  // the stub is short
}

TEST_CASE("end-of-month schedules keep month ends", "[curve][schedule]") {
    const Calendar calendar = Calendar::united_states();

    ScheduleSpec request =
        spec(Date{2024, 1, 31}, Date{2025, 1, 31}, Frequency::Quarterly);
    request.end_of_month = true;

    const Schedule schedule = make_schedule(request, calendar, DayCount::Actual360);

    REQUIRE(schedule.count == 4);
    REQUIRE(schedule[0].end == Date{2024, 4, 30});
    REQUIRE(schedule[1].end == Date{2024, 7, 31});
    REQUIRE(schedule[2].end == Date{2024, 10, 31});
    REQUIRE(schedule[3].end == Date{2025, 1, 31});
    REQUIRE(schedule[0].accrual == Catch::Approx(90.0 / 360.0));  // spans 29 Feb 2024
}

TEST_CASE("payment dates are adjusted to business days", "[curve][schedule]") {
    const Calendar calendar = Calendar::united_states();

    // 2025-01-04 is a Saturday; Modified Following moves it to Monday 2025-01-06.
    const Schedule schedule =
        make_schedule(spec(Date{2024, 1, 8}, Date{2025, 1, 4}), calendar, DayCount::Actual360);

    REQUIRE(schedule.count == 2);
    REQUIRE(schedule[0].end == Date{2024, 7, 8});
    REQUIRE(schedule.payment(1) == Date{2025, 1, 6});
}

TEST_CASE("a short front stub is the first period", "[curve][schedule]") {
    const Calendar calendar = Calendar::united_states();

    ScheduleSpec request = spec(Date{2024, 3, 8}, Date{2025, 1, 8});
    request.stub = StubPolicy::ShortFront;

    const Schedule schedule = make_schedule(request, calendar, DayCount::Actual360);

    REQUIRE(schedule.count == 2);
    REQUIRE(schedule[0].start == Date{2024, 3, 8});
    REQUIRE(schedule[0].end == Date{2024, 7, 8});
    REQUIRE(schedule[1].end == Date{2025, 1, 8});
    REQUIRE(schedule[0].accrual < schedule[1].accrual);
}
