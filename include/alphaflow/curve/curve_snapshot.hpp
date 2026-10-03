#pragma once

#include <alphaflow/curve/discount_curve.hpp>
#include <alphaflow/platform/clock.hpp>

#include <cstdint>

namespace alphaflow::curve {

/// The immutable curve published by the curve thread and read by the RFQ path.
///
/// Carries the generation and monotonic publish time the RFQ response contract
/// needs, plus the built discount curve. It is copied into a SnapshotPool slot
/// and never mutated once published.
struct CurveSnapshot {
    std::uint32_t generation{0};
    platform::Nanos published_at_ns{0};
    DiscountCurve curve;
};

}  // namespace alphaflow::curve
