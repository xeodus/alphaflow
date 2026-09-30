#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace alphaflow::concurrency {

/// Single-writer / multi-reader sequence lock (seqlock) holding one small POD.
///
/// This is the primitive behind the per-instrument quote cache
/// (ARCHITECTURE.md ADR-004). Its properties, in order of importance:
///
///   * The writer never waits for a reader. A reader never blocks the writer.
///   * A reader either observes a fully consistent snapshot or retries. It can
///     never observe a value that was never stored.
///   * Readers do not exclude each other; `load()` is wait-free when the cell
///     is quiescent.
///
/// ## Why the payload is accessed as relaxed atomics
///
/// The textbook seqlock reads and writes the payload with ordinary memory
/// operations while the writer may be active. Under the C++ memory model that
/// is a data race — undefined behaviour — even though it appears to work on
/// real hardware. This implementation instead touches every payload byte
/// through `std::atomic<std::uint8_t>` (always lock-free). All payload accesses
/// are therefore well-defined; the sequence protocol supplies the ordering.
///
/// ## Ordering
///
/// Writer (single thread):
///
///     seq.store(start + 1, relaxed);   // odd: a write is in progress
///     fence(release);                  // the odd store is ordered before the
///                                      // payload writes that follow
///     <relaxed payload writes>
///     seq.store(start + 2, release);   // even: publish. The payload writes are
///                                      // ordered before this store.
///
/// Reader (any thread):
///
///     s0 = seq.load(acquire);          // pairs with the writer's release store;
///                                      // payload reads cannot move before it
///     if (odd) retry;
///     <relaxed payload reads>
///     fence(acquire);                  // the re-read below cannot move before
///                                      // the payload reads
///     s1 = seq.load(relaxed);
///     if (s0 != s1) retry;             // a writer ran concurrently; discard
///
/// The acquire load of `s0` synchronizes with the writer's release store when it
/// observes the even value, making a completed snapshot's payload writes
/// visible. The re-read detects a writer that ran during the read; the reader
/// then discards its snapshot and tries again. This follows the fence protocol
/// described by Boehm, "Can Seqlocks Get Along With Programming Language Memory
/// Models?" (2012).
///
/// No `seq_cst` operation is used, so the hot path carries no full barrier on
/// weakly ordered architectures such as ARM.
///
/// ## A note on testing
///
/// The round-trip and sequence tests check functional behaviour. The threaded
/// stress test is a smoke detector for gross errors: because the C++ memory
/// model does not let a test observe a *hypothetical* reordering, removing one
/// of the orderings above cannot be reliably reproduced by a running test.
/// Correctness of the protocol rests on the argument above, and is backed by
/// review and the Linux ThreadSanitizer job rather than by a single assertion.
template <typename T>
class SeqlockCell {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SeqlockCell requires a trivially copyable T");

public:
    SeqlockCell() noexcept { reset(); }

    SeqlockCell(const SeqlockCell&) = delete;
    SeqlockCell& operator=(const SeqlockCell&) = delete;
    SeqlockCell(SeqlockCell&&) = delete;
    SeqlockCell& operator=(SeqlockCell&&) = delete;

    /// Writer thread only. Publishes a complete snapshot in one logical step.
    void store(const T& value) noexcept {
        const std::uint32_t start = sequence_.load(std::memory_order_relaxed);

        sequence_.store(start + 1U, std::memory_order_relaxed);  // odd
        std::atomic_thread_fence(std::memory_order_release);

        std::array<std::uint8_t, sizeof(T)> bytes{};
        std::memcpy(bytes.data(), &value, sizeof(T));
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            storage_[i].store(bytes[i], std::memory_order_relaxed);
        }

        sequence_.store(start + 2U, std::memory_order_release);  // even: publish
    }

    /// Any reader thread. One attempt: returns false if a write is in progress
    /// or completed during the read, in which case `out` is unspecified. Use
    /// this when the caller would rather skip than spin.
    [[nodiscard]] bool try_load(T& out) const noexcept {
        const std::uint32_t s0 = sequence_.load(std::memory_order_acquire);
        if ((s0 & 1U) != 0U) {
            return false;
        }

        std::array<std::uint8_t, sizeof(T)> bytes{};
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            bytes[i] = storage_[i].load(std::memory_order_relaxed);
        }

        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint32_t s1 = sequence_.load(std::memory_order_relaxed);
        if (s0 != s1) {
            return false;
        }

        std::memcpy(&out, bytes.data(), sizeof(T));
        return true;
    }

    /// Any reader thread. Spins until it observes a consistent snapshot. Never
    /// blocks the writer. Only appropriate off the latency-critical path.
    [[nodiscard]] T load() const noexcept {
        T out{};
        while (!try_load(out)) {
            // Retry: a write was in progress or raced with the read.
        }
        return out;
    }

    /// The current sequence. Even means quiescent, odd means a write is in
    /// progress. Useful for diagnostics and for the cache's dirty tracking.
    [[nodiscard]] std::uint32_t sequence() const noexcept {
        return sequence_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool write_in_progress() const noexcept {
        return (sequence() & 1U) != 0U;
    }

private:
    void reset() noexcept {
        for (std::atomic<std::uint8_t>& byte : storage_) {
            byte.store(0, std::memory_order_relaxed);
        }
    }

    static constexpr std::size_t kCacheLine = 64;

    // Per-instrument cells are padded so that one instrument's sequence cannot
    // false-share a cache line with another's.
    alignas(kCacheLine) std::atomic<std::uint32_t> sequence_{0};
    alignas(kCacheLine) mutable std::array<std::atomic<std::uint8_t>, sizeof(T)> storage_{};
};

}  // namespace alphaflow::concurrency
