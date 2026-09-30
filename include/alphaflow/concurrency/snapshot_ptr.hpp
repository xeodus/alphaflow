#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace alphaflow::concurrency {

/// Bounded pool of immutable snapshots with single-writer publication.
///
/// This is the publication mechanism of ARCHITECTURE.md §5.2 (ADR-003,
/// ADR-016). One writer thread builds a snapshot in a preallocated slot and
/// release-publishes it; any number of reader threads lease the current
/// snapshot, use it, and release. A lease keeps its snapshot immutable and
/// valid for its lifetime.
///
/// Guarantees:
///   * a reader never blocks the writer, and the writer never waits for a
///     reader -- if no slot is free, `try_publish` returns false and the caller
///     coalesces or skips (ADR-016);
///   * memory is bounded: the pool holds exactly `depth()` snapshots, ever;
///   * a leased snapshot is never overwritten, and a slot is reused only once
///     every lease on it has been released.
///
/// ## Why reference counting, and why the fences
///
/// A reader that merely loaded the published pointer and started using the
/// slot could be reading memory the writer had already handed out for reuse --
/// a use-after-free. So a reader registers itself on the slot before trusting
/// it, and the writer reclaims a slot only when its count is zero.
///
/// The subtlety is the store→load ordering between "reader increments the
/// count" and "writer reads the count". Without a barrier the writer may read
/// a stale zero and reuse a slot a reader is in the act of pinning. The
/// standard fence pairing closes this:
///
///   reader:  ptr = published.load(acquire);
///            slot->readers.fetch_add(1, acq_rel);   // pin
///            atomic_thread_fence(seq_cst);          // pair with the writer
///            if (published.load(acquire) != ptr) {  // still current?
///                slot->readers.fetch_sub(1, release); retry;
///            }
///
///   writer:  atomic_thread_fence(seq_cst);          // pair with readers
///            <scan slots for readers == 0 and not published>
///            slot->value = ...;                      // no reader can see it
///            published.store(slot, release);
///
/// The two `seq_cst` fences give a total order in which, for every reader,
/// either the writer observes the reader's pin, or the reader observes the
/// writer's publication and retries. This is the accepted reference-counting
/// reclamation protocol; it is argued here rather than tested, because a
/// running program cannot be made to exhibit a forbidden reordering.
///
/// Cost note: this makes the reader acquire two loads, one atomic RMW and one
/// fence -- more than the "≈ one acquire load" that §5.2 hoped for. Safe
/// reclamation cannot be done for less; the alternative (a long-lived pinned
/// reader) would let a stalled reader either block publication or exhaust
/// memory, both of which ADR-016 forbids. Correctness wins.
template <typename T, std::size_t PoolDepth>
class SnapshotPool {
    static_assert(PoolDepth >= 1, "SnapshotPool requires at least one slot");
    static_assert(std::is_default_constructible_v<T>,
                  "SnapshotPool requires a default-constructible T");
    static_assert(std::is_copy_assignable_v<T>,
                  "SnapshotPool requires a copy-assignable T");

public:
    static constexpr std::size_t depth() noexcept { return PoolDepth; }

private:
    // One cache line per slot so that distinct readers pinning distinct slots
    // (and the writer) do not contend on each other's counters.
    struct alignas(64) Slot {
        std::atomic<std::uint32_t> readers{0};
        T value{};
    };

public:
    /// RAII lease on the current snapshot. Move-only; releases on destruction.
    class Lease {
    public:
        Lease() noexcept = default;

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept : pool_(other.pool_), slot_(other.slot_) {
            other.pool_ = nullptr;
            other.slot_ = nullptr;
        }

        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                reset();
                pool_ = other.pool_;
                slot_ = other.slot_;
                other.pool_ = nullptr;
                other.slot_ = nullptr;
            }
            return *this;
        }

        ~Lease() { reset(); }

        /// Release the lease early. Idempotent.
        void reset() noexcept {
            if (pool_ != nullptr && slot_ != nullptr) {
                slot_->readers.fetch_sub(1, std::memory_order_release);
            }
            pool_ = nullptr;
            slot_ = nullptr;
        }

        [[nodiscard]] const T* get() const noexcept {
            return slot_ != nullptr ? &slot_->value : nullptr;
        }
        [[nodiscard]] const T& operator*() const noexcept { return slot_->value; }
        [[nodiscard]] const T* operator->() const noexcept { return &slot_->value; }
        [[nodiscard]] explicit operator bool() const noexcept { return slot_ != nullptr; }

    private:
        friend class SnapshotPool;

        Lease(SnapshotPool* pool, Slot* slot) noexcept : pool_(pool), slot_(slot) {}

        SnapshotPool* pool_{nullptr};
        Slot* slot_{nullptr};
    };

    SnapshotPool() = default;
    SnapshotPool(const SnapshotPool&) = delete;
    SnapshotPool& operator=(const SnapshotPool&) = delete;
    SnapshotPool(SnapshotPool&&) = delete;
    SnapshotPool& operator=(SnapshotPool&&) = delete;

    /// Reader thread. Returns the current snapshot, or an empty lease if
    /// nothing has been published yet. Spins only while a publish races the
    /// acquire, which is rare.
    [[nodiscard]] Lease acquire() noexcept {
        for (;;) {
            Slot* current = published_.load(std::memory_order_acquire);
            if (current == nullptr) {
                return Lease{};
            }

            current->readers.fetch_add(1, std::memory_order_acq_rel);
            std::atomic_thread_fence(std::memory_order_seq_cst);

            if (published_.load(std::memory_order_acquire) == current) {
                return Lease{this, current};
            }

            current->readers.fetch_sub(1, std::memory_order_release);
        }
    }

    /// Writer thread only. Copies `value` into a free slot and publishes it.
    /// Returns false if every slot is published or leased -- the caller
    /// coalesces and retries later. Never blocks.
    [[nodiscard]] bool try_publish(const T& value) noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);

        Slot* current = published_.load(std::memory_order_acquire);

        Slot* target = nullptr;
        for (Slot& slot : slots_) {
            if (&slot == current) {
                continue;
            }
            if (slot.readers.load(std::memory_order_acquire) == 0) {
                target = &slot;
                break;
            }
        }

        if (target == nullptr) {
            return false;
        }

        // No reader can be observing this slot: it is not published, and its
        // reader count is zero (and cannot be incremented for a slot that is
        // not published, except transiently by a racing acquire that will fail
        // its validation and not dereference).
        target->value = value;
        published_.store(target, std::memory_order_release);
        return true;
    }

private:
    std::atomic<Slot*> published_{nullptr};
    std::array<Slot, PoolDepth> slots_{};
};

}  // namespace alphaflow::concurrency
