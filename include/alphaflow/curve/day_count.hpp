#pragma once

#include <alphaflow/core/date.hpp>

#include <cstdint>

namespace alphaflow::curve {

/// Day-count convention: how a period between two dates becomes a year
/// fraction.
enum class DayCount : std::uint8_t {
    Actual360,         ///< ACT/360  — money-market and SOFR OIS fixed leg
    Actual365Fixed,    ///< ACT/365F
    ActualActualISDA,  ///< ACT/ACT (ISDA) — year-by-year actual weighting
    Thirty360,         ///< 30/360 US (bond basis)
    ThirtyE360,        ///< 30E/360 (Eurobond)
};

namespace detail {

[[nodiscard]] inline double actual_actual_isda(const core::Date& start,
                                               const core::Date& end) noexcept {
    const std::int32_t start_serial = start.serial();
    const std::int32_t end_serial = end.serial();

    double total = 0.0;
    for (int year = start.year(); year <= end.year(); ++year) {
        const std::int32_t year_start = core::Date{year, 1, 1}.serial();
        const std::int32_t year_end = core::Date{year + 1, 1, 1}.serial();

        const std::int32_t segment_start = start_serial > year_start ? start_serial : year_start;
        const std::int32_t segment_end = end_serial < year_end ? end_serial : year_end;

        if (segment_start < segment_end) {
            total += static_cast<double>(segment_end - segment_start) /
                     static_cast<double>(year_end - year_start);
        }
    }
    return total;
}

[[nodiscard]] inline int thirty_360_days(const core::Date& start, const core::Date& end,
                                         bool clamp_end_unconditionally) noexcept {
    int d1 = static_cast<int>(start.day());
    int d2 = static_cast<int>(end.day());

    if (d1 == 31) {
        d1 = 30;
    }
    if (d2 == 31 && (clamp_end_unconditionally || d1 == 30)) {
        d2 = 30;
    }

    return 360 * (end.year() - start.year()) +
           30 * (static_cast<int>(end.month()) - static_cast<int>(start.month())) + (d2 - d1);
}

}  // namespace detail

/// The year fraction of the period [start, end) under `convention`.
[[nodiscard]] inline double year_fraction(DayCount convention, const core::Date& start,
                                          const core::Date& end) noexcept {
    const double actual_days = static_cast<double>(end.serial() - start.serial());

    switch (convention) {
        case DayCount::Actual360:
            return actual_days / 360.0;
        case DayCount::Actual365Fixed:
            return actual_days / 365.0;
        case DayCount::ActualActualISDA:
            return detail::actual_actual_isda(start, end);
        case DayCount::Thirty360:
            return static_cast<double>(detail::thirty_360_days(start, end, false)) / 360.0;
        case DayCount::ThirtyE360:
            return static_cast<double>(detail::thirty_360_days(start, end, true)) / 360.0;
    }
    return 0.0;
}

}  // namespace alphaflow::curve
