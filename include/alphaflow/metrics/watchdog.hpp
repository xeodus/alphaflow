#pragma once

#include <atomic>
#include <cstdint>

namespace alphaflow::metrics {

/// A monotonic liveness counter that a thread beats periodically.
class Heartbeat {
public:
    void beat() noexcept { count_.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t count() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> count_{0};
};

/// Detects a stalled component (ARCHITECTURE.md §10): a heartbeat that has not
/// advanced since the last observation. A stalled component degrades; the
/// supervisor (external to the engine) is what restarts it.
class Watchdog {
public:
    /// Returns true if `heartbeat` advanced since `last_seen`; updates `last_seen`.
    [[nodiscard]] static bool observe(const Heartbeat& heartbeat,
                                      std::uint64_t& last_seen) noexcept {
        const std::uint64_t current = heartbeat.count();
        const bool advanced = current != last_seen;
        last_seen = current;
        return advanced;
    }
};

}  // namespace alphaflow::metrics
