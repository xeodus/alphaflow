#pragma once

#include <alphaflow/market/tick.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace alphaflow::market {

/// Single-stream arbitration over the two redundant feed lines (ADR-014, §6.2).
///
/// It is intentionally pure: it holds no locks and knows nothing about sockets,
/// so every rule below is directly testable. The Arbiter thread calls `observe`
/// for each tick drained from the two input rings and forwards the tick iff
/// `observe` returns true.
///
/// Rules:
///   * Duplicate or out-of-order sequence on a line (seq <= last for that line)
///     is dropped.
///   * The first tick seen on a line establishes its baseline.
///   * A gap on a line (seq > last + 1) marks that line stale and, if it was the
///     active line, promotes the peer; the gap tick itself is not forwarded.
///   * A stale line becomes healthy again on its next in-order tick, at which
///     point the preferred line is restored if it has recovered.
///
/// This is the M1 policy: a preferred/backup selector with immediate recovery.
/// Hysteresis and cross-line catch-up accounting are a later refinement.
class SequenceArbiter {
public:
    explicit SequenceArbiter(FeedId preferred = FeedId::A) noexcept
        : preferred_(preferred), active_(preferred) {}

    /// Returns true if the tick should be delivered downstream.
    [[nodiscard]] bool observe(const Tick& tick) noexcept {
        const std::size_t index = slot(tick.feed);
        const std::uint64_t last = last_sequence_[index];

        if (last != 0 && tick.seq <= last) {
            ++duplicates_;
            return false;
        }

        const bool first = (last == 0);
        const bool contiguous = first || (tick.seq == last + 1);

        if (!contiguous) {
            ++gaps_[index];
            healthy_[index] = false;
            last_sequence_[index] = tick.seq;
            if (active_ == tick.feed) {
                active_ = other(tick.feed);
            }
            return false;
        }

        healthy_[index] = true;
        last_sequence_[index] = tick.seq;

        if (tick.feed != active_) {
            if (tick.feed == preferred_ && healthy_[slot(preferred_)]) {
                active_ = preferred_;
            } else if (last_sequence_[slot(active_)] == 0) {
                // The active line has produced nothing yet; use this one.
                active_ = tick.feed;
            }
        }

        return tick.feed == active_;
    }

    [[nodiscard]] FeedId active() const noexcept { return active_; }
    [[nodiscard]] FeedId preferred() const noexcept { return preferred_; }

    [[nodiscard]] std::uint64_t last_sequence(FeedId feed) const noexcept {
        return last_sequence_[slot(feed)];
    }
    [[nodiscard]] std::uint64_t gap_count(FeedId feed) const noexcept {
        return gaps_[slot(feed)];
    }
    [[nodiscard]] bool healthy(FeedId feed) const noexcept { return healthy_[slot(feed)]; }
    [[nodiscard]] std::uint64_t duplicate_count() const noexcept { return duplicates_; }

private:
    [[nodiscard]] static constexpr std::size_t slot(FeedId feed) noexcept {
        return static_cast<std::size_t>(feed);
    }
    [[nodiscard]] static constexpr FeedId other(FeedId feed) noexcept {
        return feed == FeedId::A ? FeedId::B : FeedId::A;
    }

    FeedId preferred_;
    FeedId active_;
    std::array<std::uint64_t, 2> last_sequence_{0, 0};
    std::array<bool, 2> healthy_{true, true};
    std::array<std::uint64_t, 2> gaps_{0, 0};
    std::uint64_t duplicates_{0};
};

}  // namespace alphaflow::market
