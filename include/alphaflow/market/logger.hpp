#pragma once

#include <alphaflow/concurrency/spsc_ring.hpp>
#include <alphaflow/market/replay_log.hpp>
#include <alphaflow/market/tick.hpp>

#include <atomic>
#include <cstddef>
#include <thread>

namespace alphaflow::market {

/// The Logger thread (ADR-015): drains accepted ticks from a ring and appends
/// them to the replay log. Runs at low priority, off the ingest/arbitration hot
/// path; the arbiter never blocks on it.
template <std::size_t Capacity>
class Logger {
public:
    using Ring = concurrency::SpscRing<Tick, Capacity>;

    Logger(Ring& ring, ReplayLog& log) noexcept : ring_(ring), log_(log) {}

    /// Append every available tick once. Returns how many were written.
    std::size_t drain_once() noexcept {
        Tick tick;
        std::size_t written = 0;
        while (ring_.try_pop(tick)) {
            if (log_.append(tick)) {
                ++written;
            }
        }
        written_ += written;
        return written;
    }

    void run(const std::atomic<bool>& stop) noexcept {
        while (!stop.load(std::memory_order_relaxed)) {
            drain_once();
            std::this_thread::yield();
        }
        drain_once();
    }

    [[nodiscard]] std::size_t written() const noexcept { return written_; }

private:
    Ring& ring_;
    ReplayLog& log_;
    std::size_t written_{0};
};

}  // namespace alphaflow::market
