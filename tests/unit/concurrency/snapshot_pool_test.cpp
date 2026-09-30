// Unit tests for alphaflow::concurrency::SnapshotPool.
//
// The pool is the immutable-snapshot publication mechanism of
// ARCHITECTURE.md §5.2 (ADR-003, ADR-016): a single writer publishes snapshots
// into a bounded, preallocated pool; any number of readers lease the current
// one; a leased snapshot stays valid for the lease's lifetime.
//
// The properties these tests pin down are exactly the ones the architecture
// promises and that CAN be observed by execution:
//
//   * a lease yields a consistent snapshot that never changes under the
//     reader, even as the writer keeps publishing;
//   * the pool is bounded: when every slot is published or leased,
//     try_publish refuses rather than overwriting;
//   * a stalled reader never blocks the writer or makes the writer wait --
//     the writer is told "no free slot" and moves on;
//   * releasing a lease makes its slot reusable;
//   * leases move and release exactly once;
//   * under real threads, readers never observe a torn or freed snapshot.
//
// The memory-ordering protocol that makes reclamation safe is argued in the
// header, not tested here: no running program can be made to exhibit a
// reordering the memory model forbids.

#include <alphaflow/concurrency/snapshot_ptr.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace {

using alphaflow::concurrency::SnapshotPool;

struct Curve {
    std::uint64_t generation;
    double invariant;  // always generation * 2.0
};

}  // namespace

TEST_CASE("a fresh pool has no snapshot to lease", "[concurrency][snapshot]") {
    SnapshotPool<Curve, 4> pool;

    auto lease = pool.acquire();
    REQUIRE_FALSE(lease);
    REQUIRE(lease.get() == nullptr);
}

TEST_CASE("publishing makes a snapshot available", "[concurrency][snapshot]") {
    SnapshotPool<Curve, 4> pool;

    REQUIRE(pool.try_publish(Curve{1, 2.0}));

    auto lease = pool.acquire();
    REQUIRE(lease);
    REQUIRE(lease->generation == 1);
    REQUIRE(lease->invariant == 2.0);
}

TEST_CASE("a lease sees the latest snapshot at acquire time", "[concurrency][snapshot]") {
    SnapshotPool<Curve, 4> pool;

    REQUIRE(pool.try_publish(Curve{1, 2.0}));
    REQUIRE(pool.try_publish(Curve{2, 4.0}));
    REQUIRE(pool.try_publish(Curve{3, 6.0}));

    auto lease = pool.acquire();
    REQUIRE(lease->generation == 3);
}

TEST_CASE("a held lease is stable while the writer advances", "[concurrency][snapshot]") {
    SnapshotPool<Curve, 4> pool;

    REQUIRE(pool.try_publish(Curve{1, 2.0}));
    auto held = pool.acquire();  // pins generation 1

    REQUIRE(pool.try_publish(Curve{2, 4.0}));
    REQUIRE(pool.try_publish(Curve{3, 6.0}));

    REQUIRE(held->generation == 1);  // the reader's view never moved
    REQUIRE(held->invariant == 2.0);

    auto fresh = pool.acquire();
    REQUIRE(fresh->generation == 3);
}

TEST_CASE("the pool is bounded and publishing refuses when exhausted",
          "[concurrency][snapshot]") {
    SnapshotPool<Curve, 3> pool;

    REQUIRE(pool.try_publish(Curve{1, 2.0}));  // slot A, published
    auto lease_a = pool.acquire();             // pins A
    REQUIRE(pool.try_publish(Curve{2, 4.0}));  // slot B, published
    auto lease_b = pool.acquire();             // pins B
    REQUIRE(pool.try_publish(Curve{3, 6.0}));  // slot C, published
    auto lease_c = pool.acquire();             // pins C

    // A, B and C are all held; C is also the published slot. No slot is free.
    REQUIRE_FALSE(pool.try_publish(Curve{4, 8.0}));

    // Releasing one lease frees its slot for reuse.
    lease_a.reset();
    REQUIRE(pool.try_publish(Curve{4, 8.0}));

    REQUIRE(lease_b->generation == 2);  // other leases stay valid
    REQUIRE(lease_c->generation == 3);

    auto latest = pool.acquire();
    REQUIRE(latest->generation == 4);
}

TEST_CASE("a stalled reader does not block the writer, and its view stays valid",
          "[concurrency][snapshot]") {
    SnapshotPool<Curve, 2> pool;

    REQUIRE(pool.try_publish(Curve{1, 2.0}));  // slot A, published
    auto stalled = pool.acquire();             // pins A and never lets go

    REQUIRE(pool.try_publish(Curve{2, 4.0}));  // slot B, published

    // Both slots are unavailable: A is pinned, B is published. The writer is
    // told so instead of waiting.
    REQUIRE_FALSE(pool.try_publish(Curve{3, 6.0}));

    // The stalled reader's snapshot is untouched.
    REQUIRE(stalled->generation == 1);
    REQUIRE(stalled->invariant == 2.0);

    stalled.reset();
    REQUIRE(pool.try_publish(Curve{3, 6.0}));

    auto latest = pool.acquire();
    REQUIRE(latest->generation == 3);
}

TEST_CASE("leases move and release exactly once", "[concurrency][snapshot]") {
    SnapshotPool<Curve, 2> pool;
    REQUIRE(pool.try_publish(Curve{1, 2.0}));

    auto source = pool.acquire();
    REQUIRE(source);

    auto moved = std::move(source);
    REQUIRE_FALSE(source);
    REQUIRE(moved);
    REQUIRE(moved->generation == 1);

    moved.reset();
    REQUIRE_FALSE(moved);
    moved.reset();  // idempotent; must not double-release
}

TEST_CASE("concurrent readers never observe a torn snapshot",
          "[concurrency][snapshot]") {
    constexpr std::uint64_t kPublished = 20'000;

    SnapshotPool<Curve, 8> pool;

    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};

    std::vector<std::thread> readers;
    readers.reserve(3);
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&pool, &stop, &torn] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto lease = pool.acquire();
                if (lease) {
                    const Curve& curve = *lease;
                    if (curve.invariant != static_cast<double>(curve.generation) * 2.0) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    std::uint64_t written = 0;
    while (written < kPublished) {
        const std::uint64_t generation = written + 1;
        if (pool.try_publish(Curve{generation, static_cast<double>(generation) * 2.0})) {
            ++written;
        }
        // Otherwise every slot is momentarily leased; the writer simply tries
        // again, which is the non-blocking contract.
    }

    stop.store(true, std::memory_order_relaxed);
    for (std::thread& reader : readers) {
        reader.join();
    }

    REQUIRE(torn.load() == 0);
}
