#pragma once

namespace alphaflow::curve {

/// SR1/SR3 futures imply a rate: a SOFR future quotes `100·(1 − R)`, so the
/// implied rate is `(100 − price)/100`.
[[nodiscard]] inline double futures_implied_rate(double futures_price) noexcept {
    return (100.0 - futures_price) / 100.0;
}

/// Convexity adjustment for a futures-implied forward over [start, end] years.
///
/// Hull–White with zero mean reversion: `δ = ½·σ²·t₁·t₂`. This is a documented
/// *approximation* — the exact adjustment depends on the rate model, and matching
/// QuantLib's futures helper would require its model and parameters
/// (ARCHITECTURE.md §7.2). The futures forward must be adjusted before use or
/// the curve kinks at the futures/swap junction.
[[nodiscard]] inline double futures_convexity_adjustment(double volatility, double start_years,
                                                         double end_years) noexcept {
    return 0.5 * volatility * volatility * start_years * end_years;
}

/// The convexity-adjusted forward rate implied by a futures price.
[[nodiscard]] inline double adjusted_futures_rate(double futures_price, double volatility,
                                                  double start_years,
                                                  double end_years) noexcept {
    return futures_implied_rate(futures_price) +
           futures_convexity_adjustment(volatility, start_years, end_years);
}

}  // namespace alphaflow::curve
