#include <alphaflow/curve/schedule.hpp>

#include <array>

namespace alphaflow::curve {
namespace {

/// Roll `date` by `months`, snapping to the month end when requested.
core::Date roll(const core::Date& date, int months, bool end_of_month) noexcept {
    const core::Date next = date.add_months(months);
    if (!end_of_month) {
        return next;
    }
    return core::Date{next.year(), next.month(),
                      core::Date::days_in_month(next.year(), next.month())};
}

}  // namespace

core::Date spot_date(const core::Date& trade, const Calendar& calendar, int lag) noexcept {
    return calendar.advance(trade, lag);
}

Schedule make_schedule(const ScheduleSpec& spec, const Calendar& calendar,
                       DayCount day_count) noexcept {
    Schedule schedule;
    schedule.day_count = day_count;

    const int tenor_months = 12 / static_cast<int>(spec.frequency);

    std::array<core::Date, Schedule::kMaxPeriods + 1> unadjusted{};
    std::size_t count = 0;

    if (spec.stub == StubPolicy::ShortBack) {
        unadjusted[count++] = spec.effective;
        core::Date cursor = spec.effective;
        while (count < Schedule::kMaxPeriods) {
            const core::Date next = roll(cursor, tenor_months, spec.end_of_month);
            if (next >= spec.termination) {
                break;
            }
            unadjusted[count++] = next;
            cursor = next;
        }
        if (unadjusted[count - 1] != spec.termination) {
            unadjusted[count++] = spec.termination;
        }
    } else {  // StubPolicy::ShortFront
        std::array<core::Date, Schedule::kMaxPeriods + 1> backward{};
        std::size_t backward_count = 0;
        backward[backward_count++] = spec.termination;

        core::Date cursor = spec.termination;
        while (backward_count < Schedule::kMaxPeriods) {
            const core::Date previous = roll(cursor, -tenor_months, spec.end_of_month);
            if (previous <= spec.effective) {
                break;
            }
            backward[backward_count++] = previous;
            cursor = previous;
        }
        if (backward[backward_count - 1] != spec.effective) {
            backward[backward_count++] = spec.effective;
        }
        for (std::size_t i = 0; i < backward_count; ++i) {
            unadjusted[i] = backward[backward_count - 1 - i];
        }
        count = backward_count;
    }

    for (std::size_t i = 0; i + 1 < count; ++i) {
        const core::Date start = calendar.adjust(unadjusted[i], spec.convention);
        const core::Date end = calendar.adjust(unadjusted[i + 1], spec.convention);

        Period period;
        period.start = start;
        period.end = end;
        period.payment = end;
        period.accrual = year_fraction(day_count, start, end);
        schedule.periods[schedule.count++] = period;
    }

    return schedule;
}

}  // namespace alphaflow::curve
