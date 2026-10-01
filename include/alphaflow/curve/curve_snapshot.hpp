#pragma once

#include <alphaflow/platform/clock.hpp>

#include <cstdint>

namespace alphaflow::curve {

/// M1 skeleton curve snapshot.
///
/// The real snapshot -- discount factors, pillars, the interpolation and the
/// conventions it was built with -- lands in Phase 3. This placeholder carries
/// only what the RFQ path needs to run end to end: a generation counter, the
/// monotonic time it was published, and one flat rate. It is trivially
/// copyable so it can live in a SnapshotPool slot, and it is deliberately the
/// only curve type the RFQ plumbing knows about (via a template parameter), so
/// Phase 3 can replace it without touching the server.
struct CurveSnapshot {
    std::uint32_t generation{0};
    platform::Nanos published_at_ns{0};
    double par_rate{0.0};

    bool operator==(const CurveSnapshot&) const = default;
};

}  // namespace alphaflow::curve
