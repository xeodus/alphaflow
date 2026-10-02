#pragma once

#include <alphaflow/curve/discount_curve.hpp>
#include <alphaflow/curve/schedule.hpp>

#include <cstddef>

namespace alphaflow::curve::ois {

/// OIS swap valuation given a schedule and a discount curve.
///
/// These implement the identities of ARCHITECTURE.md §7.5 and are pure,
/// noexcept, and allocation-free (fixed-capacity inputs). The single-curve OIS
/// convention is assumed: the same curve projects SOFR and discounts, there is
/// no spread, and each payment falls on its accrual end.

/// Fixed-leg annuity per unit notional: `Σ_j δ_j · D(pay_j)`.
[[nodiscard]] inline double annuity(const Schedule& schedule,
                                    const DiscountCurve& curve) noexcept {
    double total = 0.0;
    for (std::size_t i = 0; i < schedule.count; ++i) {
        total += schedule.periods[i].accrual * curve.discount(schedule.periods[i].payment);
    }
    return total;
}

/// Compounded-OIS floating leg, telescoped: `N · (D(t0) − D(tn))`.
[[nodiscard]] inline double floating_leg_pv(const Schedule& schedule,
                                            const DiscountCurve& curve,
                                            double notional) noexcept {
    if (schedule.count == 0) {
        return 0.0;
    }
    const double start_df = curve.discount(schedule.periods[0].start);
    const double end_df = curve.discount(schedule.periods[schedule.count - 1].end);
    return notional * (start_df - end_df);
}

/// Fixed-leg PV: `N · K · annuity`.
[[nodiscard]] inline double fixed_leg_pv(const Schedule& schedule,
                                         const DiscountCurve& curve, double fixed_rate,
                                         double notional) noexcept {
    return notional * fixed_rate * annuity(schedule, curve);
}

/// Par OIS rate: `(D(t0) − D(tn)) / annuity`.
[[nodiscard]] inline double par_rate(const Schedule& schedule,
                                     const DiscountCurve& curve) noexcept {
    const double denominator = annuity(schedule, curve);
    if (denominator == 0.0) {
        return 0.0;
    }
    return floating_leg_pv(schedule, curve, 1.0) / denominator;
}

/// PV to the fixed-rate receiver: `fixed_leg_pv − floating_leg_pv`.
[[nodiscard]] inline double pv_receive_fixed(const Schedule& schedule,
                                             const DiscountCurve& curve, double fixed_rate,
                                             double notional) noexcept {
    return fixed_leg_pv(schedule, curve, fixed_rate, notional) -
           floating_leg_pv(schedule, curve, notional);
}

}  // namespace alphaflow::curve::ois
