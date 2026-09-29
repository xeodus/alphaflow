#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace alphaflow::concurrency {

/// Single-producer / single-consumer lock-free ring buffer.
///
/// This is the transport between the market-data producer and the curve
/// consumer. It is deliberately narrow: one producer thread and one consumer
/// thread, no CAS, no retry loop, no contention.
///
/// Contract:
///   * Exactly one thread may call try_push (the producer).
///   * Exactly one thread may call try_pop (the consumer).
///   * Capacity must be a power of two and at least 2.
///   * T must be default-constructible and copy-assignable.
///
/// Memory ordering: the producer publishes the head with release and the
/// consumer acquires it; the consumer publishes the tail with release and the
/// producer acquires it. No sequence-consistent operations are used, so the
/// hot path pays no full-barrier cost on weakly ordered architectures (ARM).
///
/// A full ring refuses the push rather than overwriting unread data. The
/// full-queue *policy* (coalesce, drop, shed) belongs to the caller; the
/// engine coalesces upstream in the quote cache, so this primitive is only
/// ever asked to carry change-notifications.
///
/// size()/empty()/full() are advisory snapshots intended for diagnostics and
/// single-threaded reasoning; they are not a synchronisation mechanism.
template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2, "SpscRing capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "SpscRing capacity must be a power of two");
    static_assert(std::is_default_constructible_v<T>,
                  "SpscRing element type must be default-constructible");
    static_assert(std::is_copy_assignable_v<T>,
                  "SpscRing element type must be copy-assignable");

public:
    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    /// Producer thread only. Returns false if the ring is full.
    [[nodiscard]] bool try_push(const T& value) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail == Capacity) {
            return false;
        }
        buffer_[head & kMask] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Consumer thread only. Returns false if the ring is empty.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        out = buffer_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        return head - tail;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] bool full() const noexcept { return size() == Capacity; }

private:
    static constexpr std::size_t kMask = Capacity - 1;
    static constexpr std::size_t kCacheLine = 64;

    // head_ and tail_ are written by different threads, so they must never
    // share a cache line. The buffer is padded away from both for the same
    // reason.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLine) std::array<T, Capacity> buffer_{};
};

}  // namespace alphaflow::concurrency
