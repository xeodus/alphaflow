#pragma once

#include <alphaflow/platform/clock.hpp>

#include <cstdint>

namespace alphaflow::market {

/// Identifier of an instrument in the quote cache (index into it).
using InstrumentId = std::uint32_t;

/// The two redundant feed lines. A is the preferred line.
enum class FeedId : std::uint8_t { A = 0, B = 1 };

/// A normalized market-data update, timestamped on arrival before any parsing
/// or allocation so the ingest latency span stays honest (ARCHITECTURE.md §6.1).
struct Tick {
    InstrumentId id{};
    double value{};               ///< rate or price, normalized units
    std::uint64_t seq{};          ///< per-feed sequence for gap detection
    platform::Ticks recv_tsc{};   ///< monotonic capture at arrival
    FeedId feed{FeedId::A};
};

}  // namespace alphaflow::market
