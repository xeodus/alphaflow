#pragma once

#include <alphaflow/curve/curve_snapshot.hpp>
#include <alphaflow/curve/schedule.hpp>

namespace alphaflow::pricing {

/// A vanilla fixed-vs-OIS swap, with its schedule precomputed once.
struct SwapSpec {
    curve::Schedule schedule;
    double notional{0.0};
    double fixed_rate{0.0};
};

/// The result of pricing a swap. `pv` is the value to the fixed-rate receiver;
/// `par_rate` is the rate at which `pv` is zero.
struct Quote {
    double pv{0.0};
    double par_rate{0.0};
};

/// Price a swap off an immutable curve snapshot. Stateless, noexcept, and
/// allocation-free (the schedule is already built).
[[nodiscard]] Quote price_swap(const curve::CurveSnapshot& snapshot,
                               const SwapSpec& spec) noexcept;

/// DV01 by parallel bump-and-revalue of the zero curve (per 1 bp). Signed:
/// negative for a fixed-rate receiver, whose value falls when rates rise.
[[nodiscard]] double dv01(const curve::CurveSnapshot& snapshot,
                          const SwapSpec& spec) noexcept;

}  // namespace alphaflow::pricing
