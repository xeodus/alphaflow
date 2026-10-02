#pragma once

#include <alphaflow/core/date.hpp>

#include <array>
#include <cmath>
#include <cstddef>

namespace alphaflow::curve {

/// A discount curve as a small, fixed-capacity set of pillars, interpolated
/// **log-linearly in time**.
///
/// Log-linear interpolation on discount factors gives positive discount
/// factors, piecewise-constant forward rates, no arbitrage, and — crucially —
/// **local support**: the value at `t` in `[t_i, t_{i+1}]` depends only on
/// pillars `i` and `i+1`. That locality is what makes the incremental rebuild
/// of ARCHITECTURE.md §7.4 correct.
///
/// Time is measured in years from `reference` using ACT/365F, the curve's
/// internal axis. The struct is a plain value so a fully-built curve can live
/// inside an immutable snapshot.
struct DiscountCurve {
    static constexpr std::size_t kMaxPillars = 128;

    std::array<double, kMaxPillars> times{};    ///< year fractions, strictly increasing
    std::array<double, kMaxPillars> log_dfs{};  ///< ln D(t) at each pillar
    std::size_t count{0};
    core::Date reference{};

    /// ln D(t), log-linearly interpolated; the boundary segment is extended
    /// linearly outside [times[0], times[count-1]] (flat forward).
    [[nodiscard]] double log_discount(double t) const noexcept {
        if (count == 0) {
            return 0.0;
        }
        if (count == 1) {
            return log_dfs[0];
        }
        if (t <= times[0]) {
            return segment(0, t);
        }
        if (t >= times[count - 1]) {
            return segment(count - 2, t);
        }
        std::size_t i = 0;
        while (i + 1 < count && times[i + 1] < t) {
            ++i;
        }
        return segment(i, t);
    }

    [[nodiscard]] double discount(double t) const noexcept {
        return std::exp(log_discount(t));
    }

    [[nodiscard]] double discount(const core::Date& date) const noexcept {
        return discount(time_of(date));
    }

    /// Continuously-compounded zero rate over [0, t].
    [[nodiscard]] double zero_rate(double t) const noexcept {
        return t > 0.0 ? -log_discount(t) / t : 0.0;
    }

    /// Continuously-compounded forward rate over [t0, t1].
    [[nodiscard]] double forward(double t0, double t1) const noexcept {
        return t1 > t0 ? (log_discount(t0) - log_discount(t1)) / (t1 - t0) : 0.0;
    }

    /// ACT/365F year fraction of `date` from the reference.
    [[nodiscard]] double time_of(const core::Date& date) const noexcept {
        return static_cast<double>(date.serial() - reference.serial()) / 365.0;
    }

private:
    [[nodiscard]] double segment(std::size_t i, double t) const noexcept {
        const double slope = (log_dfs[i + 1] - log_dfs[i]) / (times[i + 1] - times[i]);
        return log_dfs[i] + slope * (t - times[i]);
    }
};

}  // namespace alphaflow::curve
