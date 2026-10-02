#pragma once

#include <alphaflow/core/date.hpp>

#include <cstdint>

namespace alphaflow::curve {

/// How a date that is not a business day is moved onto one (ARCHITECTURE.md
/// §7.6). Modified Following is the market default for schedules.
enum class BusinessDayConvention : std::uint8_t {
    Following,          ///< next business day
    ModifiedFollowing,  ///< next business day, unless it crosses a month end
    Preceding,          ///< previous business day
    ModifiedPreceding,  ///< previous business day, unless it crosses a month end
    Unadjusted,         ///< unchanged
};

/// A business-day calendar.
///
/// M1 provides the US Government Bond (SIFMA) calendar — the standard for USD
/// rates swaps and the one QuantLib's `UnitedStates::GovernmentBond` matches.
/// It observes the Federal Reserve/SIFMA holidays including Good Friday,
/// Columbus Day and Veterans Day; the NY Fed (Fedwire) calendar differs and is
/// a later configuration option.
class Calendar {
public:
    [[nodiscard]] static Calendar united_states() noexcept { return Calendar{}; }

    [[nodiscard]] bool is_weekend(const core::Date& date) const noexcept {
        const core::Weekday day = date.weekday();
        return day == core::Weekday::Saturday || day == core::Weekday::Sunday;
    }

    [[nodiscard]] bool is_holiday(const core::Date& date) const noexcept {
        const int year = date.year();

        // Fixed-date holidays, with the weekend observance rule. New Year's is
        // checked for the current and the following year because a Saturday
        // 1 January is observed on 31 December of the prior year.
        if (date == observed(year, 1, 1) || date == observed(year + 1, 1, 1)) {
            return true;
        }
        if (date == observed(year, 7, 4)) {
            return true;
        }
        if (date == observed(year, 12, 25)) {
            return true;
        }
        if (date == observed(year, 11, 11)) {
            return true;
        }
        if (year >= 2021 && date == observed(year, 6, 19)) {
            return true;
        }

        // Floating holidays.
        if (date == nth_weekday(year, 1, core::Weekday::Monday, 3)) {
            return true;
        }
        if (date == nth_weekday(year, 2, core::Weekday::Monday, 3)) {
            return true;
        }
        if (date == last_weekday(year, 5, core::Weekday::Monday)) {
            return true;
        }
        if (date == nth_weekday(year, 9, core::Weekday::Monday, 1)) {
            return true;
        }
        if (date == nth_weekday(year, 10, core::Weekday::Monday, 2)) {
            return true;
        }
        if (date == nth_weekday(year, 11, core::Weekday::Thursday, 4)) {
            return true;
        }
        if (date == easter(year).add_days(-2)) {  // Good Friday
            return true;
        }

        return false;
    }

    [[nodiscard]] bool is_business_day(const core::Date& date) const noexcept {
        return !is_weekend(date) && !is_holiday(date);
    }

    [[nodiscard]] core::Date adjust(const core::Date& date,
                                    BusinessDayConvention convention) const noexcept {
        switch (convention) {
            case BusinessDayConvention::Unadjusted:
                return date;
            case BusinessDayConvention::Following:
                return roll(date, +1, false);
            case BusinessDayConvention::Preceding:
                return roll(date, -1, false);
            case BusinessDayConvention::ModifiedFollowing:
                return roll(date, +1, true);
            case BusinessDayConvention::ModifiedPreceding:
                return roll(date, -1, true);
        }
        return date;
    }

    /// Move `business_days` business days forward (positive) or back (negative),
    /// stepping one calendar day at a time and counting only business days.
    [[nodiscard]] core::Date advance(const core::Date& date, int business_days) const noexcept {
        if (business_days == 0) {
            return date;
        }
        const int step = business_days > 0 ? 1 : -1;
        int remaining = business_days > 0 ? business_days : -business_days;

        core::Date result = date;
        while (remaining > 0) {
            result = result.add_days(step);
            if (is_business_day(result)) {
                --remaining;
            }
        }
        return result;
    }

private:
    Calendar() noexcept = default;

    [[nodiscard]] core::Date roll(const core::Date& date, int step,
                                  bool keep_month) const noexcept {
        core::Date result = date;
        while (!is_business_day(result)) {
            result = result.add_days(step);
        }
        if (keep_month && result.month() != date.month()) {
            result = date;
            while (!is_business_day(result)) {
                result = result.add_days(-step);
            }
        }
        return result;
    }

    [[nodiscard]] static core::Date observed(int year, unsigned month, unsigned day) noexcept {
        const core::Date date{year, month, day};
        if (date.weekday() == core::Weekday::Saturday) {
            return date.add_days(-1);
        }
        if (date.weekday() == core::Weekday::Sunday) {
            return date.add_days(1);
        }
        return date;
    }

    [[nodiscard]] static core::Date nth_weekday(int year, unsigned month, core::Weekday target,
                                                int n) noexcept {
        const core::Date first{year, month, 1};
        const int offset =
            (static_cast<int>(target) - static_cast<int>(first.weekday()) + 7) % 7;
        return core::Date{year, month, static_cast<unsigned>(1 + offset + (n - 1) * 7)};
    }

    [[nodiscard]] static core::Date last_weekday(int year, unsigned month,
                                                 core::Weekday target) noexcept {
        const unsigned last_day = core::Date::days_in_month(year, month);
        const core::Date last{year, month, last_day};
        const int offset =
            (static_cast<int>(last.weekday()) - static_cast<int>(target) + 7) % 7;
        return core::Date{year, month, last_day - static_cast<unsigned>(offset)};
    }

    /// Easter Sunday by the Anonymous Gregorian computus; Good Friday is two
    /// days earlier.
    [[nodiscard]] static core::Date easter(int year) noexcept {
        const int a = year % 19;
        const int b = year / 100;
        const int c = year % 100;
        const int d = b / 4;
        const int e = b % 4;
        const int f = (b + 8) / 25;
        const int g = (b - f + 1) / 3;
        const int h = (19 * a + b - d - g + 15) % 30;
        const int i = c / 4;
        const int k = c % 4;
        const int l = (32 + 2 * e + 2 * i - h - k) % 7;
        const int m = (a + 11 * h + 22 * l) / 451;
        const int month = (h + l - 7 * m + 114) / 31;
        const int day = ((h + l - 7 * m + 114) % 31) + 1;
        return core::Date{year, static_cast<unsigned>(month), static_cast<unsigned>(day)};
    }
};

}  // namespace alphaflow::curve
