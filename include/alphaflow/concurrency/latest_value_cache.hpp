#pragma once

#include <alphaflow/concurrency/seqlock.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace alphaflow::concurrency {

/// Fixed-capacity table of latest-value cells, indexed by instrument.
///
/// This is the quote cache of ARCHITECTURE.md §5.3. The arbiter thread is the
/// single writer; the curve thread (and any other reader) reads the latest
/// value per instrument without locking and without blocking the writer.
///
/// ## Change detection and the no-lost-change invariant
///
/// A reader learns that an instrument changed by observing that its *epoch*
/// has advanced. The epoch is the number of completed writes to that slot: it
/// increments by exactly one per `update`, is monotonic, and is derived from
/// the seqlock's own sequence counter (even sequence / 2), so it costs nothing
/// to maintain and cannot disagree with the published value.
///
/// This is deliberately *not* a boolean "dirty" flag that is set on write and
/// cleared on read. That pattern has a lost-update race: if the writer stores
/// again between the reader's decision to process and its clear, the clear
/// erases the newer change. Because the epoch is monotonic, a reader that
/// remembers the last epoch it processed can never miss an update -- an
/// instrument whose epoch has advanced past the remembered value *has* changed,
/// no matter how many notifications were coalesced or dropped upstream. This is
/// the guarantee the architecture states: "an instrument whose write_epoch
/// advanced is processed even if its notification was coalesced."
///
/// The typical reader keeps a parallel array of last-seen epochs and processes
/// the slots whose epoch differs. `epoch()` reads the last *completed* write
/// (while a write is in flight the epoch still reads the previous value), so a
/// reader is never told about a value it cannot yet load consistently.
template <typename T, std::size_t Capacity>
class LatestValueCache {
    static_assert(Capacity >= 1, "LatestValueCache requires at least one slot");

public:
    LatestValueCache() = default;
    LatestValueCache(const LatestValueCache&) = delete;
    LatestValueCache& operator=(const LatestValueCache&) = delete;
    LatestValueCache(LatestValueCache&&) = delete;
    LatestValueCache& operator=(LatestValueCache&&) = delete;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    /// Writer thread only. Publishes `value` to `index` and advances its epoch.
    void update(std::size_t index, const T& value) noexcept {
        assert(index < Capacity);
        cells_[index].store(value);
    }

    /// Any reader thread. One attempt; false if a write is in progress or raced.
    [[nodiscard]] bool try_load(std::size_t index, T& out) const noexcept {
        assert(index < Capacity);
        return cells_[index].try_load(out);
    }

    /// Any reader thread. Spins until a consistent snapshot is observed.
    [[nodiscard]] T load(std::size_t index) const noexcept {
        assert(index < Capacity);
        return cells_[index].load();
    }

    /// The number of completed writes to `index` (monotonic, +1 per update).
    [[nodiscard]] std::uint32_t epoch(std::size_t index) const noexcept {
        assert(index < Capacity);
        return cells_[index].sequence() >> 1U;
    }

private:
    std::array<SeqlockCell<T>, Capacity> cells_{};
};

}  // namespace alphaflow::concurrency
