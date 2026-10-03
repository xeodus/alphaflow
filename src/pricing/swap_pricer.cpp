#include <alphaflow/pricing/swap_pricer.hpp>

#include <alphaflow/curve/ois_instrument.hpp>

#include <cstddef>

namespace alphaflow::pricing {

Quote price_swap(const curve::CurveSnapshot& snapshot, const SwapSpec& spec) noexcept {
    Quote quote;
    quote.pv = curve::ois::pv_receive_fixed(spec.schedule, snapshot.curve, spec.fixed_rate,
                                            spec.notional);
    quote.par_rate = curve::ois::par_rate(spec.schedule, snapshot.curve);
    return quote;
}

double dv01(const curve::CurveSnapshot& snapshot, const SwapSpec& spec) noexcept {
    constexpr double kShift = 1e-4;  // 1 bp, so (pv_up - pv_down)/2 is per bp

    curve::CurveSnapshot up = snapshot;
    curve::CurveSnapshot down = snapshot;
    for (std::size_t i = 0; i < snapshot.curve.count; ++i) {
        // D(t) -> D(t)·exp(-δ·t) is a parallel +δ bump of the zero curve.
        up.curve.log_dfs[i] -= kShift * up.curve.times[i];
        down.curve.log_dfs[i] += kShift * down.curve.times[i];
    }

    const double pv_up =
        curve::ois::pv_receive_fixed(spec.schedule, up.curve, spec.fixed_rate, spec.notional);
    const double pv_down =
        curve::ois::pv_receive_fixed(spec.schedule, down.curve, spec.fixed_rate, spec.notional);

    return (pv_up - pv_down) / 2.0;
}

}  // namespace alphaflow::pricing
