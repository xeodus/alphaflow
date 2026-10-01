#pragma once

#include <alphaflow/platform/clock.hpp>
#include <alphaflow/rfq/protocol.hpp>

#include <cstdint>

namespace alphaflow::rfq {

/// The set of instruments the server will quote. M1 uses a contiguous range;
/// a real instrument registry replaces this in a later milestone.
struct InstrumentUniverse {
    std::uint32_t count{0};

    [[nodiscard]] bool contains(std::uint32_t instrument_id) const noexcept {
        return instrument_id < count;
    }
};

/// Maps a request and the current snapshot to a response (ARCHITECTURE.md
/// §10.1). Pure: no I/O, no allocation, no blocking.
///
/// The snapshot type is a template parameter so the RFQ plumbing is independent
/// of the curve implementation; a snapshot must expose `generation` and
/// `published_at_ns`.
template <typename Snapshot>
class Responder {
public:
    /// A price as a function of the instrument and the snapshot. A plain
    /// function pointer keeps the hot path free of virtual dispatch and
    /// allocation.
    using PriceFn = double (*)(std::uint32_t instrument_id, const Snapshot& snapshot) noexcept;

    Responder(InstrumentUniverse universe, platform::Nanos staleness_threshold_ns,
              PriceFn price) noexcept
        : universe_(universe),
          staleness_threshold_ns_(staleness_threshold_ns),
          price_(price) {}

    /// `snapshot` is the currently leased snapshot, or null before the first
    /// publish. `now_ns` is the caller's monotonic timestamp, passed in rather
    /// than read here so the responder stays pure and testable.
    [[nodiscard]] Response respond(const Request& request, const Snapshot* snapshot,
                                   platform::Nanos now_ns) const noexcept {
        Response response;
        response.request_id = request.request_id;

        if (snapshot == nullptr) {
            response.status = Status::WarmingUp;
            return response;
        }

        response.generation = snapshot->generation;

        if (!universe_.contains(request.instrument_id)) {
            response.status = Status::UnknownInstrument;
            return response;
        }

        response.price = price_(request.instrument_id, *snapshot);
        response.status = (now_ns - snapshot->published_at_ns > staleness_threshold_ns_)
                              ? Status::Stale
                              : Status::Ok;
        return response;
    }

private:
    InstrumentUniverse universe_;
    platform::Nanos staleness_threshold_ns_;
    PriceFn price_;
};

}  // namespace alphaflow::rfq
