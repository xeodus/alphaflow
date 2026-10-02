#pragma once

#include <alphaflow/core/date.hpp>
#include <alphaflow/curve/calendar.hpp>
#include <alphaflow/curve/day_count.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace alphaflow::curve {

/// Coupon frequency, as the number of payments per year.
enum class Frequency : std::uint8_t {
    Annual = 1,
    SemiAnnual = 2,
    Quarterly = 4,
    Monthly = 12,
};

/// Where a non-regular stub period sits. M1 builds short stubs; long stubs are
/// a later addition (no M1 instrument requires them).
enum class StubPolicy : std::uint8_t {
    ShortFront,
    ShortBack,
};

struct ScheduleSpec {
    core::Date effective;
    core::Date termination;
    Frequency frequency{Frequency::SemiAnnual};
    BusinessDayConvention convention{BusinessDayConvention::ModifiedFollowing};
    bool end_of_month{false};
    StubPolicy stub{StubPolicy::ShortBack};
};

/// One accrual period. `start`/`end` are the business-day-adjusted accrual
/// dates; `payment` is the coupon payment date; `accrual` is the day-count year
/// fraction of [start, end].
struct Period {
    core::Date start;
    core::Date end;
    core::Date payment;
    double accrual{0.0};
};

/// A fully generated, fixed-capacity schedule. Generated once per instrument;
/// pricing only evaluates it, never regenerates it (ARCHITECTURE.md §7.6).
struct Schedule {
    static constexpr std::size_t kMaxPeriods = 256;

    std::array<Period, kMaxPeriods> periods{};
    std::size_t count{0};
    DayCount day_count{DayCount::Actual360};

    [[nodiscard]] const Period& operator[](std::size_t index) const noexcept {
        return periods[index];
    }
    [[nodiscard]] const core::Date& payment(std::size_t index) const noexcept {
        return periods[index].payment;
    }
};

/// T+2 (or `lag` business days) spot date from a trade date.
[[nodiscard]] core::Date spot_date(const core::Date& trade, const Calendar& calendar,
                                   int lag = 2) noexcept;

/// Generate a deterministic schedule from `spec`. The regular grid is spaced by
/// 12/frequency months from `effective`; a non-regular final (or first) period
/// is a short stub. Every date is adjusted to a business day by the convention,
/// and month ends are preserved when `end_of_month` is set.
[[nodiscard]] Schedule make_schedule(const ScheduleSpec& spec, const Calendar& calendar,
                                     DayCount day_count) noexcept;

}  // namespace alphaflow::curve
