// Unit tests for alphaflow::concurrency::SeqlockCell.
//
// The seqlock is the primitive behind the per-instrument quote cache
// (ARCHITECTURE.md ADR-004): one writer, many readers, no reader ever blocks
// the writer, and a reader either observes a fully-consistent snapshot or
// retries.
//
// The tests are written to fail loudly if the sequence protocol is broken:
//
//   * store/load round-trips values of several sizes and layouts;
//   * the sequence advances by two per store and is even when quiescent;
//   * try_load succeeds on a quiescent cell;
//   * under real threads, readers never observe a torn value (a field
//     inconsistent with another) and never observe time going backwards,
//     while a writer hammers the cell. This is the test that matters: it
//     detects a missing fence or a lost sequence check.

#include <alphaflow/concurrency/seqlock.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using alphaflow::concurrency::SeqlockCell;

struct Small {
    double value;
};

struct Pair16 {
    std::uint64_t a;
    std::uint64_t b;
};

struct Wide {
    std::uint64_t a;
    std::uint64_t b;
    std::uint64_t c;
};

}  // namespace

TEMPLATE_TEST_CASE("store then load round-trips a POD layout",
                   "[concurrency][seqlock]", Small, Pair16, Wide) {
    SeqlockCell<TestType> cell;

    TestType value{};
    auto* raw = reinterpret_cast<std::uint8_t*>(&value);
    for (std::size_t i = 0; i < sizeof(TestType); ++i) {
        raw[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }

    cell.store(value);

    TestType out{};
    REQUIRE(cell.try_load(out));
    REQUIRE(std::memcmp(&value, &out, sizeof(TestType)) == 0);
}

TEST_CASE("a default-constructed cell is readable and zeroed", "[concurrency][seqlock]") {
    SeqlockCell<Pair16> cell;

    REQUIRE_FALSE(cell.write_in_progress());

    Pair16 out{};
    out.a = 1;
    out.b = 2;
    REQUIRE(cell.try_load(out));
    REQUIRE(out.a == 0);
    REQUIRE(out.b == 0);
}

TEST_CASE("sequence advances by two per store and stays even", "[concurrency][seqlock]") {
    SeqlockCell<Small> cell;

    const std::uint32_t start = cell.sequence();
    REQUIRE_FALSE(cell.write_in_progress());

    cell.store(Small{1.5});
    REQUIRE(cell.sequence() == start + 2);
    REQUIRE_FALSE(cell.write_in_progress());

    cell.store(Small{2.5});
    REQUIRE(cell.sequence() == start + 4);
    REQUIRE_FALSE(cell.write_in_progress());
}

TEST_CASE("load returns the most recently stored value", "[concurrency][seqlock]") {
    SeqlockCell<Small> cell;

    for (int i = 1; i <= 100; ++i) {
        cell.store(Small{static_cast<double>(i)});
    }

    REQUIRE(cell.load().value == 100.0);
}

TEST_CASE("readers never observe a torn or regressed value under a real writer",
          "[concurrency][seqlock]") {
    constexpr std::uint64_t kWrites = 200'000;

    SeqlockCell<Pair16> cell;
    cell.store(Pair16{0, ~std::uint64_t{0}});

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> torn_reads{0};
    std::atomic<std::uint64_t> regressions{0};

    std::vector<std::thread> readers;
    readers.reserve(3);
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&cell, &stop, &torn_reads, &regressions] {
            std::uint64_t last = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const Pair16 value = cell.load();
                if (value.b != ~value.a) {
                    torn_reads.fetch_add(1, std::memory_order_relaxed);
                }
                if (value.a < last) {
                    regressions.fetch_add(1, std::memory_order_relaxed);
                }
                last = value.a;
            }
        });
    }

    for (std::uint64_t i = 1; i <= kWrites; ++i) {
        cell.store(Pair16{i, ~i});
    }

    stop.store(true, std::memory_order_relaxed);
    for (std::thread& reader : readers) {
        reader.join();
    }

    REQUIRE(torn_reads.load() == 0);
    REQUIRE(regressions.load() == 0);
    REQUIRE(cell.load().a == kWrites);
}
