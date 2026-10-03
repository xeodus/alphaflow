#pragma once

#include <alphaflow/core/date.hpp>
#include <alphaflow/curve/calendar.hpp>
#include <alphaflow/curve/day_count.hpp>
#include <alphaflow/curve/discount_curve.hpp>
#include <alphaflow/curve/schedule.hpp>

#include <cstddef>
#include <span>

namespace alphaflow::curve {

/// A par OIS instrument that pins one curve pillar.
struct OisPillar {
    core::Date maturity;
    double par_rate{0.0};
};

struct BootstrapSpec {
    core::Date reference;  ///< curve t=0 (the spot date); D(reference) = 1
    Frequency frequency{Frequency::Annual};
    DayCount fixed_day_count{DayCount::Actual360};
    BusinessDayConvention convention{BusinessDayConvention::ModifiedFollowing};
    bool end_of_month{false};
    double tolerance{1e-12};
    int max_iterations{50};
};

struct BootstrapResult {
    DiscountCurve curve;
    std::size_t pillars{0};
    std::size_t iterations{0};
    bool converged{true};
};

/// Sequential OIS bootstrap (ARCHITECTURE.md §7.2), short to long.
///
/// For each pillar, in increasing maturity, solve the discount factor that makes
/// that pillar's par OIS swap price to zero given the already-solved pillars.
/// The OIS pillar equation is linear in the last discount factor, so the Newton
/// step (analytic derivative) converges in a single iteration; `max_iterations`
/// bounds it regardless. Pillars must be ordered by increasing maturity.
///
/// Runs on the curve thread (off the RFQ hot path), so it is not required to be
/// allocation-free; it is noexcept and uses fixed-capacity storage.
[[nodiscard]] BootstrapResult bootstrap(std::span<const OisPillar> pillars,
                                        const Calendar& calendar,
                                        const BootstrapSpec& spec) noexcept;

/// Re-solve the curve in place from pillar `first_changed` onward, keeping the
/// already-solved earlier pillars. Because interpolation is local and the
/// bootstrap is sequential, only pillars at or after `first_changed` can change;
/// this is the correct incremental boundary (ADR-008). The curve must already
/// have `pillars.size() + 1` entries. Returns false if any re-solve fails.
[[nodiscard]] bool rebuild_from(DiscountCurve& curve, std::size_t first_changed,
                                std::span<const OisPillar> pillars,
                                const Calendar& calendar,
                                const BootstrapSpec& spec) noexcept;

}  // namespace alphaflow::curve
