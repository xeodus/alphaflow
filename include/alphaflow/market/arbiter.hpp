#pragma once

#include <alphaflow/concurrency/latest_value_cache.hpp>
#include <alphaflow/concurrency/spsc_ring.hpp>
#include <alphaflow/market/sequence_arbiter.hpp>
#include <alphaflow/market/tick.hpp>

#include <atomic>
#include <cstddef>
#include <thread>

namespace alphaflow::market {

/// The Arbiter thread's logic (ADR-014, §4.1): the sole writer of the quote
/// cache.
///
/// It drains the two feed rings (one per line), runs each tick through the
/// `SequenceArbiter`, and for every accepted tick updates the cache and pushes a
/// change notification. It is the fix for the producer-count contradiction: the
/// feed threads never share mutable state; only this thread writes the cache.
///
/// Not thread-safe by itself; `run` is the thread body, `drain_once` is the
/// single-threaded core used by tests.
template <std::size_t CacheCapacity, std::size_t NotificationCapacity,
          std::size_t InputCapacity>
class Arbiter {
public:
    using InputRing = concurrency::SpscRing<Tick, InputCapacity>;
    using NotificationRing = concurrency::SpscRing<InstrumentId, NotificationCapacity>;
    using Cache = concurrency::LatestValueCache<double, CacheCapacity>;

    Arbiter(InputRing& ring_a, InputRing& ring_b, Cache& cache,
            NotificationRing& notifications) noexcept
        : ring_a_(ring_a),
          ring_b_(ring_b),
          cache_(cache),
          notifications_(notifications) {}

    /// Drain every available tick from both lines exactly once.
    void drain_once() noexcept {
        Tick tick;
        while (ring_a_.try_pop(tick)) {
            dispatch(tick);
        }
        while (ring_b_.try_pop(tick)) {
            dispatch(tick);
        }
    }

    /// Thread body: drain continuously until `stop`, then a final drain.
    void run(const std::atomic<bool>& stop) noexcept {
        while (!stop.load(std::memory_order_relaxed)) {
            drain_once();
            std::this_thread::yield();
        }
        drain_once();
    }

    [[nodiscard]] const SequenceArbiter& sequences() const noexcept { return sequences_; }

private:
    void dispatch(const Tick& tick) noexcept {
        if (!sequences_.observe(tick)) {
            return;
        }
        cache_.update(tick.id, tick.value);
        // A full notification ring is not an error: the cache epoch still
        // advanced, so a consumer that re-checks the epoch cannot miss the
        // change (the no-lost-change invariant, §5.3 / ADR-016).
        static_cast<void>(notifications_.try_push(tick.id));
    }

    InputRing& ring_a_;
    InputRing& ring_b_;
    Cache& cache_;
    NotificationRing& notifications_;
    SequenceArbiter sequences_;
};

}  // namespace alphaflow::market
