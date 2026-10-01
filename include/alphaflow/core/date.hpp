#pragma once

#include <compare>
#include <cstdint>

namespace alphaflow::core {

/// Day of week, ISO-ordered (Monday = 1 .. Sunday = 7).
enum class Weekday : std::uint8_t {
    Monday = 1,
    Tuesday = 2,
    Wednesday = 3,
    Thursday = 4,
    Friday = 5,
    Saturday = 6,
    Sunday = 7,
};

/// A proleptic-Gregorian civil date.
///
/// The serial day number is days since 1970-01-01 (serial 0), computed with
/// Howard Hinnant's `days_from_civil` / `civil_from_days` algorithms. This is
/// the arithmetic every day count and schedule builds on, so it is exact and
/// handles dates before the epoch (negative serials).
///
/// Construction does not validate its arguments; schedule generation is
/// responsible for producing valid dates. `days_in_month` and `is_leap_year`
/// are exposed for callers that need to reason about month ends.
class Date {
public:
    constexpr Date() noexcept = default;
    constexpr Date(int year, unsigned month, unsigned day) noexcept
        : year_(year), month_(month), day_(day) {}

    [[nodiscard]] static constexpr bool is_leap_year(int year) noexcept {
        return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
    }

    [[nodiscard]] static constexpr unsigned days_in_month(int year, unsigned month) noexcept {
        switch (month) {
            case 1:
            case 3:
            case 5:
            case 7:
            case 8:
            case 10:
            case 12:
                return 31;
            case 4:
            case 6:
            case 9:
            case 11:
                return 30;
            case 2:
                return is_leap_year(year) ? 29U : 28U;
            default:
                return 0;
        }
    }

    [[nodiscard]] static constexpr Date from_serial(std::int32_t serial) noexcept {
        const std::int32_t z = serial + 719468;
        const std::int32_t era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = static_cast<unsigned>(z - era * 146097);            // [0, 146096]
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
        const int year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);            // [0, 365]
        const unsigned mp = (5 * doy + 2) / 153;                                 // [0, 11]
        const unsigned day = doy - (153 * mp + 2) / 5 + 1;                       // [1, 31]
        const int month = static_cast<int>(mp) + (mp < 10 ? 3 : -9);             // [1, 12]
        return Date{year + (month <= 2 ? 1 : 0), static_cast<unsigned>(month), day};
    }

    [[nodiscard]] constexpr std::int32_t serial() const noexcept {
        std::int32_t year = year_;
        const unsigned month = month_;
        year -= (month <= 2 ? 1 : 0);
        const std::int32_t era = (year >= 0 ? year : year - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(year - era * 400);            // [0, 399]
        const int mp = (month > 2) ? static_cast<int>(month) - 3
                                   : static_cast<int>(month) + 9;               // [0, 11]
        const unsigned doy = static_cast<unsigned>((153 * mp + 2) / 5) + day_ - 1;  // [0, 365]
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;              // [0, 146096]
        return era * 146097 + static_cast<std::int32_t>(doe) - 719468;
    }

    [[nodiscard]] constexpr int year() const noexcept { return year_; }
    [[nodiscard]] constexpr unsigned month() const noexcept { return month_; }
    [[nodiscard]] constexpr unsigned day() const noexcept { return day_; }

    /// 1970-01-01 (serial 0) was a Thursday.
    [[nodiscard]] constexpr Weekday weekday() const noexcept {
        const std::int32_t w = (((serial() + 3) % 7) + 7) % 7;  // 0 = Monday
        return static_cast<Weekday>(w + 1);
    }

    [[nodiscard]] constexpr bool is_end_of_month() const noexcept {
        return day_ == days_in_month(year_, month_);
    }

    [[nodiscard]] constexpr Date add_days(std::int64_t days) const noexcept {
        return from_serial(serial() + static_cast<std::int32_t>(days));
    }

    /// Add calendar months, clamping the day to the target month's length
    /// (2024-01-31 + 1 month = 2024-02-29). End-of-month roll conventions are
    /// applied by schedule generation, not here.
    [[nodiscard]] constexpr Date add_months(int months) const noexcept {
        const int total = year_ * 12 + static_cast<int>(month_) - 1 + months;
        const int target_year = floor_div(total, 12);
        const int target_month = total - target_year * 12 + 1;
        const unsigned month = static_cast<unsigned>(target_month);
        const unsigned length = days_in_month(target_year, month);
        return Date{target_year, month, day_ < length ? day_ : length};
    }

    [[nodiscard]] constexpr bool operator==(const Date& other) const noexcept = default;
    [[nodiscard]] constexpr std::strong_ordering operator<=>(const Date& other) const noexcept = default;

private:
    [[nodiscard]] static constexpr int floor_div(int numerator, int denominator) noexcept {
        const int quotient = numerator / denominator;
        const bool exact = (numerator % denominator) == 0;
        const bool signs_differ = (numerator < 0) != (denominator < 0);
        return (exact || !signs_differ) ? quotient : quotient - 1;
    }

    int year_{1970};
    unsigned month_{1};
    unsigned day_{1};
};

}  // namespace alphaflow::core
