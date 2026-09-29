// Unit tests for alphaflow::concurrency::SpscRing.
//
// The ring is the transport between the market-data producer and the curve
// consumer, so its contract is fixed before the implementation:
//
//   * a fresh ring is empty and refuses to pop;
//   * values come out in FIFO order;
//   * the ring holds exactly `capacity()` elements and then refuses to push
//     (it never overwrites unread data);
//   * indices wrap correctly;
//   * under a randomized single-thread schedule it matches a reference model;
//   * a real producer and consumer thread move a long sequence across the
//     boundary intact (checked under ThreadSanitizer on Linux CI).

#include <alphaflow/concurrency/spsc_ring.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using alphaflow::concurrency::SpscRing;

// The capacity is a compile-time contract.
static_assert(SpscRing<std::uint32_t, 4>::capacity() == 4);

}  // namespace

TEST_CASE("a fresh ring is empty", "[concurrency][spsc]") {
    SpscRing<std::uint32_t, 8> ring;

    REQUIRE(ring.empty());
    REQUIRE(ring.size() == 0);
    REQUIRE_FALSE(ring.full());

    std::uint32_t out = 0;
    REQUIRE_FALSE(ring.try_pop(out));
}

TEST_CASE("push then pop preserves the value (FIFO)", "[concurrency][spsc]") {
    SpscRing<std::uint32_t, 8> ring;

    REQUIRE(ring.try_push(42));
    REQUIRE_FALSE(ring.empty());
    REQUIRE(ring.size() == 1);

    std::uint32_t out = 0;
    REQUIRE(ring.try_pop(out));
    REQUIRE(out == 42);
    REQUIRE(ring.empty());
}

TEST_CASE("ring holds capacity elements then reports full", "[concurrency][spsc]") {
    constexpr std::size_t kCapacity = 8;
    SpscRing<std::uint32_t, kCapacity> ring;

    for (std::uint32_t i = 0; i < kCapacity; ++i) {
        REQUIRE(ring.try_push(i));
    }
    REQUIRE(ring.full());
    REQUIRE(ring.size() == kCapacity);
    REQUIRE_FALSE(ring.try_push(999));  // must not overwrite unread data

    for (std::uint32_t i = 0; i < kCapacity; ++i) {
        std::uint32_t out = 0;
        REQUIRE(ring.try_pop(out));
        REQUIRE(out == i);
    }
    REQUIRE(ring.empty());
    REQUIRE_FALSE(ring.full());
}

TEST_CASE("ring survives index wraparound", "[concurrency][spsc]") {
    SpscRing<std::uint32_t, 4> ring;

    std::uint32_t next_push = 0;
    std::uint32_t next_pop = 0;

    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 3; ++i) {
            REQUIRE(ring.try_push(next_push));
            ++next_push;
        }
        for (int i = 0; i < 3; ++i) {
            std::uint32_t out = 0;
            REQUIRE(ring.try_pop(out));
            REQUIRE(out == next_pop);
            ++next_pop;
        }
    }
    REQUIRE(ring.empty());
}

TEST_CASE("randomized schedule matches a reference FIFO", "[concurrency][spsc]") {
    constexpr std::size_t kCapacity = 16;
    SpscRing<std::uint32_t, kCapacity> ring;

    std::vector<std::uint32_t> reference;
    std::uint32_t next_value = 0;

    // A fixed LCG keeps the schedule deterministic across platforms.
    std::uint64_t state = 0x1234'5678'9abc'def0ULL;
    const auto next_random = [&state]() {
        state = state * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
        return static_cast<std::uint32_t>(state >> 33);
    };

    for (int step = 0; step < 200'000; ++step) {
        const bool want_push =
            reference.size() < kCapacity && (reference.empty() || (next_random() & 1U) != 0U);

        if (want_push) {
            REQUIRE(ring.try_push(next_value));
            reference.push_back(next_value);
            ++next_value;
        } else {
            REQUIRE_FALSE(reference.empty());
            std::uint32_t out = 0;
            REQUIRE(ring.try_pop(out));
            REQUIRE(out == reference.front());
            reference.erase(reference.begin());
        }
    }
}

TEST_CASE("single producer and consumer transfer a sequence intact",
          "[concurrency][spsc]") {
    constexpr std::uint64_t kCount = 1'000'000;
    SpscRing<std::uint64_t, 1024> ring;

    std::thread producer([&ring] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!ring.try_push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::uint64_t expected = 0;
    std::uint64_t received = 0;
    bool order_ok = true;

    while (received < kCount) {
        std::uint64_t value = 0;
        if (ring.try_pop(value)) {
            if (value != expected) {
                order_ok = false;
            }
            ++expected;
            ++received;
        } else {
            std::this_thread::yield();
        }
    }

    producer.join();

    REQUIRE(order_ok);
    REQUIRE(received == kCount);
    REQUIRE(ring.empty());
}
