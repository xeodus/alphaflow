// Unit tests for alphaflow::concurrency::LatestValueCache.
//
// The cache is the source of truth the curve thread prices from: the arbiter
// writes the latest quote per instrument and the curve thread reads it. Its
// central obligation is the no-lost-change invariant from ARCHITECTURE.md
// §5.3 -- a change notification may be coalesced or dropped, but no completed
// write may ever become invisible.
//
// The tests:
//
//   * a fresh cache is readable, zeroed, at epoch zero;
//   * update/load round-trips per slot, including multi-field payloads;
//   * each update advances that slot's epoch by exactly one;
//   * slots are independent;
//   * coalesced updates (many writes with no intervening read) are not lost:
//     one observation of the epoch reveals them all;
//   * under a real writer, readers never see a regressed epoch or value.

#include <alphaflow/concurrency/latest_value_cache.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using alphaflow::concurrency::LatestValueCache;

struct Quote {
    double value;
    std::uint32_t source;
    std::uint32_t reserved;
};

}  // namespace

TEST_CASE("a fresh cache is zeroed and at epoch zero", "[concurrency][cache]") {
    LatestValueCache<std::uint64_t, 4> cache;

    std::uint64_t out = 12345;
    REQUIRE(cache.try_load(0, out));
    REQUIRE(out == 0);

    REQUIRE(cache.epoch(0) == 0);
    REQUIRE(cache.epoch(3) == 0);
}

TEST_CASE("update then load round-trips and slots stay independent",
          "[concurrency][cache]") {
    LatestValueCache<std::uint64_t, 4> cache;

    cache.update(1, 11);
    cache.update(2, 22);

    REQUIRE(cache.load(0) == 0);
    REQUIRE(cache.load(1) == 11);
    REQUIRE(cache.load(2) == 22);

    REQUIRE(cache.epoch(0) == 0);
    REQUIRE(cache.epoch(1) == 1);
    REQUIRE(cache.epoch(2) == 1);
}

TEST_CASE("each update advances that slot's epoch by one", "[concurrency][cache]") {
    LatestValueCache<std::uint64_t, 4> cache;

    for (std::uint32_t i = 1; i <= 5; ++i) {
        cache.update(2, i);
        REQUIRE(cache.epoch(2) == i);
        REQUIRE(cache.load(2) == i);
    }
    REQUIRE(cache.epoch(0) == 0);
}

TEST_CASE("coalesced updates are not lost", "[concurrency][cache]") {
    LatestValueCache<std::uint64_t, 4> cache;

    const std::uint32_t seen_before = cache.epoch(0);
    REQUIRE(seen_before == 0);

    // Ten writes with no intervening read: every intermediate notification
    // could have been coalesced away.
    for (std::uint64_t i = 1; i <= 10; ++i) {
        cache.update(0, i * 100);
    }

    // A single observation must reveal that a change happened, and to which
    // epoch, without needing the individual notifications.
    const std::uint32_t seen_after = cache.epoch(0);
    REQUIRE(seen_after != seen_before);
    REQUIRE(seen_after == 10);
    REQUIRE(cache.load(0) == 1000);

    // Quiescent: no phantom change.
    REQUIRE(cache.epoch(0) == seen_after);
}

TEST_CASE("a multi-field payload round-trips", "[concurrency][cache]") {
    LatestValueCache<Quote, 2> cache;

    cache.update(1, Quote{1.5, 7, 0});

    Quote out{};
    REQUIRE(cache.try_load(1, out));
    REQUIRE(out.value == 1.5);
    REQUIRE(out.source == 7);
    REQUIRE(cache.epoch(1) == 1);
}

TEST_CASE("readers never observe a regressed epoch or value", "[concurrency][cache]") {
    constexpr std::uint64_t kWrites = 200'000;

    LatestValueCache<std::uint64_t, 8> cache;

    std::atomic<bool> stop{false};
    std::atomic<int> regressions{0};

    std::vector<std::thread> readers;
    readers.reserve(3);
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&cache, &stop, &regressions] {
            std::uint32_t last_epoch = 0;
            std::uint64_t last_value = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const std::uint32_t epoch = cache.epoch(3);
                const std::uint64_t value = cache.load(3);
                if (epoch < last_epoch || value < last_value) {
                    regressions.fetch_add(1, std::memory_order_relaxed);
                }
                last_epoch = epoch;
                last_value = value;
            }
        });
    }

    for (std::uint64_t i = 1; i <= kWrites; ++i) {
        cache.update(3, i);
    }

    stop.store(true, std::memory_order_relaxed);
    for (std::thread& reader : readers) {
        reader.join();
    }

    REQUIRE(regressions.load() == 0);
    REQUIRE(cache.epoch(3) == kWrites);
    REQUIRE(cache.load(3) == kWrites);
}
