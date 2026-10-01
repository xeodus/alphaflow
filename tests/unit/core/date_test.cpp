// Unit tests for alphaflow::core::Date.
//
// Date arithmetic is the foundation of every day count and schedule, and a
// silent error here propagates into every price. The tests pin the serial-day
// epoch, round-trip conversion, weekday, chronological ordering, and the two
// arithmetic operations schedules rely on: add_days and add_months (with
// month-end clamping).

#include <alphaflow/core/date.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {

using alphaflow::core::Date;
using alphaflow::core::Weekday;

}  // namespace

TEST_CASE("a date exposes its civil fields", "[core][date]") {
    const Date date{2024, 2, 29};
    REQUIRE(date.year() == 2024);
    REQUIRE(date.month() == 2);
    REQUIRE(date.day() == 29);
}

TEST_CASE("leap years follow the Gregorian rule", "[core][date]") {
    REQUIRE(Date::is_leap_year(2024));
    REQUIRE_FALSE(Date::is_leap_year(2023));
    REQUIRE_FALSE(Date::is_leap_year(1900));  // divisible by 100, not 400
    REQUIRE(Date::is_leap_year(2000));        // divisible by 400

    REQUIRE(Date::days_in_month(2024, 2) == 29);
    REQUIRE(Date::days_in_month(2023, 2) == 28);
    REQUIRE(Date::days_in_month(2024, 4) == 30);
    REQUIRE(Date::days_in_month(2024, 1) == 31);
}

TEST_CASE("the epoch and its neighbours have the expected serials", "[core][date]") {
    REQUIRE(Date{1970, 1, 1}.serial() == 0);
    REQUIRE(Date{1970, 1, 2}.serial() == 1);
    REQUIRE(Date{1969, 12, 31}.serial() == -1);
    REQUIRE(Date{2000, 1, 1}.serial() == 10957);
}

TEST_CASE("serial conversion round-trips over four decades", "[core][date]") {
    for (std::int32_t serial = Date{1970, 1, 1}.serial();
         serial <= Date{2030, 1, 1}.serial(); ++serial) {
        REQUIRE(Date::from_serial(serial).serial() == serial);
    }
}

TEST_CASE("weekdays are correct", "[core][date]") {
    REQUIRE(Date{1970, 1, 1}.weekday() == Weekday::Thursday);
    REQUIRE(Date{1970, 1, 5}.weekday() == Weekday::Monday);
    REQUIRE(Date{2000, 1, 1}.weekday() == Weekday::Saturday);
    REQUIRE(Date{2024, 2, 29}.weekday() == Weekday::Thursday);
}

TEST_CASE("dates order chronologically", "[core][date]") {
    REQUIRE(Date{2023, 12, 31} < Date{2024, 1, 1});
    REQUIRE(Date{2024, 2, 29} < Date{2024, 3, 1});
    REQUIRE(Date{2024, 1, 1} == Date{2024, 1, 1});
    REQUIRE(Date{2024, 1, 2} > Date{2024, 1, 1});
}

TEST_CASE("add_days crosses month, year, and leap boundaries", "[core][date]") {
    REQUIRE(Date{2024, 2, 28}.add_days(1) == Date{2024, 2, 29});
    REQUIRE(Date{2024, 2, 29}.add_days(1) == Date{2024, 3, 1});
    REQUIRE(Date{2023, 2, 28}.add_days(1) == Date{2023, 3, 1});
    REQUIRE(Date{2024, 1, 1}.add_days(-1) == Date{2023, 12, 31});
    REQUIRE(Date{2024, 12, 31}.add_days(1) == Date{2025, 1, 1});
}

TEST_CASE("add_months clamps to the target month length", "[core][date]") {
    REQUIRE(Date{2024, 1, 31}.add_months(1) == Date{2024, 2, 29});
    REQUIRE(Date{2023, 1, 31}.add_months(1) == Date{2023, 2, 28});
    REQUIRE(Date{2024, 2, 29}.add_months(12) == Date{2025, 2, 28});
    REQUIRE(Date{2024, 1, 15}.add_months(2) == Date{2024, 3, 15});
    REQUIRE(Date{2024, 1, 15}.add_months(-13) == Date{2022, 12, 15});
    REQUIRE(Date{2024, 11, 30}.add_months(3) == Date{2025, 2, 28});
}

TEST_CASE("end-of-month is recognised", "[core][date]") {
    REQUIRE(Date{2024, 2, 29}.is_end_of_month());
    REQUIRE_FALSE(Date{2024, 2, 28}.is_end_of_month());
    REQUIRE(Date{2023, 2, 28}.is_end_of_month());
    REQUIRE(Date{2024, 4, 30}.is_end_of_month());
    REQUIRE_FALSE(Date{2024, 4, 29}.is_end_of_month());
}
