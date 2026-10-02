// Unit tests for alphaflow::curve::DayCount.
//
// Day counts convert a period into a year fraction. They are pure date
// arithmetic but easy to get subtly wrong, so each convention is checked
// against hand-computed values, including the boundary cases that distinguish
// the variants (month-end days, period boundaries, leap years).

#include <alphaflow/curve/day_count.hpp>

#include <alphaflow/core/date.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

using alphaflow::core::Date;
using alphaflow::curve::DayCount;
using alphaflow::curve::year_fraction;

}  // namespace

TEST_CASE("ACT/360 is actual days over 360", "[curve][daycount]") {
    REQUIRE(year_fraction(DayCount::Actual360, Date{2024, 1, 1}, Date{2024, 7, 1}) ==
            Catch::Approx(182.0 / 360.0));
    REQUIRE(year_fraction(DayCount::Actual360, Date{2024, 1, 1}, Date{2024, 1, 1}) ==
            Catch::Approx(0.0));
}

TEST_CASE("ACT/365F is actual days over 365", "[curve][daycount]") {
    REQUIRE(year_fraction(DayCount::Actual365Fixed, Date{2024, 1, 1}, Date{2024, 7, 1}) ==
            Catch::Approx(182.0 / 365.0));
}

TEST_CASE("ACT/ACT ISDA splits at year boundaries and weights by year length",
          "[curve][daycount]") {
    // 184 days in 2023 (365) plus 182 days in 2024 (366).
    REQUIRE(year_fraction(DayCount::ActualActualISDA, Date{2023, 7, 1}, Date{2024, 7, 1}) ==
            Catch::Approx(184.0 / 365.0 + 182.0 / 366.0));
    // within a single leap year
    REQUIRE(year_fraction(DayCount::ActualActualISDA, Date{2024, 1, 1}, Date{2024, 7, 1}) ==
            Catch::Approx(182.0 / 366.0));
    // exactly one year is 1.0 whether or not it is a leap year
    REQUIRE(year_fraction(DayCount::ActualActualISDA, Date{2023, 1, 1}, Date{2024, 1, 1}) ==
            Catch::Approx(1.0));
    REQUIRE(year_fraction(DayCount::ActualActualISDA, Date{2024, 1, 1}, Date{2025, 1, 1}) ==
            Catch::Approx(1.0));
}

TEST_CASE("30/360 US applies the day-of-month rules", "[curve][daycount]") {
    // both ends are the 31st: both become 30
    REQUIRE(year_fraction(DayCount::Thirty360, Date{2024, 1, 31}, Date{2024, 7, 31}) ==
            Catch::Approx(180.0 / 360.0));
    // d1 = 15: d2 = 31 stays 31
    REQUIRE(year_fraction(DayCount::Thirty360, Date{2024, 1, 15}, Date{2024, 3, 31}) ==
            Catch::Approx(76.0 / 360.0));
    // d1 = 31 -> 30, then d2 = 31 -> 30
    REQUIRE(year_fraction(DayCount::Thirty360, Date{2024, 1, 31}, Date{2024, 3, 31}) ==
            Catch::Approx(60.0 / 360.0));
}

TEST_CASE("30E/360 clamps the end day unconditionally", "[curve][daycount]") {
    REQUIRE(year_fraction(DayCount::ThirtyE360, Date{2024, 1, 31}, Date{2024, 7, 31}) ==
            Catch::Approx(180.0 / 360.0));
    // d2 = 31 -> 30 regardless of d1
    REQUIRE(year_fraction(DayCount::ThirtyE360, Date{2024, 1, 15}, Date{2024, 3, 31}) ==
            Catch::Approx(75.0 / 360.0));
}

TEST_CASE("the actual conventions sign consistently when dates are reversed",
          "[curve][daycount]") {
    REQUIRE(year_fraction(DayCount::Actual360, Date{2024, 7, 1}, Date{2024, 1, 1}) ==
            Catch::Approx(-182.0 / 360.0));
}
