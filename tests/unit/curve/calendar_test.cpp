// Unit tests for alphaflow::curve::Calendar.
//
// The business-day calendar is the other silent-bug source (with day counts):
// wrong holidays or a wrong roll convention shift every accrual and payment
// date. These tests pin the US Government Bond (SIFMA) calendar, the weekend
// observance rule, Modified Following across a month end, and business-day
// stepping.

#include <alphaflow/curve/calendar.hpp>

#include <alphaflow/core/date.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace {

using alphaflow::core::Date;
using alphaflow::curve::BusinessDayConvention;
using alphaflow::curve::Calendar;

}  // namespace

TEST_CASE("weekends are not business days", "[curve][calendar]") {
    const Calendar calendar = Calendar::united_states();

    REQUIRE(calendar.is_weekend(Date{2024, 1, 6}));   // Saturday
    REQUIRE(calendar.is_weekend(Date{2024, 1, 7}));   // Sunday
    REQUIRE_FALSE(calendar.is_business_day(Date{2024, 1, 6}));
    REQUIRE_FALSE(calendar.is_business_day(Date{2024, 1, 7}));
    REQUIRE(calendar.is_business_day(Date{2024, 1, 8}));  // Monday
}

TEST_CASE("US Government Bond holidays are recognised in 2024", "[curve][calendar]") {
    const Calendar calendar = Calendar::united_states();

    const std::array<Date, 12> holidays = {{
        {2024, 1, 1},    // New Year's Day
        {2024, 1, 15},   // Martin Luther King Jr. Day
        {2024, 2, 19},   // Washington's Birthday
        {2024, 3, 29},   // Good Friday
        {2024, 5, 27},   // Memorial Day
        {2024, 6, 19},   // Juneteenth
        {2024, 7, 4},    // Independence Day
        {2024, 9, 2},    // Labor Day
        {2024, 10, 14},  // Columbus Day
        {2024, 11, 11},  // Veterans Day
        {2024, 11, 28},  // Thanksgiving
        {2024, 12, 25},  // Christmas
    }};

    for (const Date& holiday : holidays) {
        REQUIRE(calendar.is_holiday(holiday));
        REQUIRE_FALSE(calendar.is_business_day(holiday));
    }
}

TEST_CASE("fixed holidays observe the weekend rule", "[curve][calendar]") {
    const Calendar calendar = Calendar::united_states();

    REQUIRE(calendar.is_holiday(Date{2021, 12, 24}));  // Christmas (Sat) -> Fri
    REQUIRE(calendar.is_holiday(Date{2021, 12, 31}));  // New Year (Sat) -> prior Fri
    REQUIRE(calendar.is_holiday(Date{2021, 7, 5}));    // Independence (Sun) -> Mon
}

TEST_CASE("Modified Following rolls back across a month end", "[curve][calendar]") {
    const Calendar calendar = Calendar::united_states();
    const Date end_of_august{2024, 8, 31};  // Saturday

    // Following would land on Mon 2024-09-03 (skipping Labor Day), next month.
    REQUIRE(calendar.adjust(end_of_august, BusinessDayConvention::Following) ==
            Date{2024, 9, 3});
    // Modified Following pulls back into August.
    REQUIRE(calendar.adjust(end_of_august, BusinessDayConvention::ModifiedFollowing) ==
            Date{2024, 8, 30});
    REQUIRE(calendar.adjust(end_of_august, BusinessDayConvention::Preceding) ==
            Date{2024, 8, 30});
    REQUIRE(calendar.adjust(end_of_august, BusinessDayConvention::Unadjusted) ==
            end_of_august);

    // A business day is unchanged by any convention.
    REQUIRE(calendar.adjust(Date{2024, 8, 30}, BusinessDayConvention::ModifiedFollowing) ==
            Date{2024, 8, 30});
}

TEST_CASE("advance steps over weekends and holidays", "[curve][calendar]") {
    const Calendar calendar = Calendar::united_states();

    REQUIRE(calendar.advance(Date{2024, 1, 5}, 1) == Date{2024, 1, 8});   // Fri -> Mon
    REQUIRE(calendar.advance(Date{2024, 1, 8}, -1) == Date{2024, 1, 5});  // Mon -> Fri
    // Sat -> skip MLK Monday -> Tuesday.
    REQUIRE(calendar.advance(Date{2024, 1, 13}, 1) == Date{2024, 1, 16});
    REQUIRE(calendar.advance(Date{2024, 1, 5}, 0) == Date{2024, 1, 5});
}
