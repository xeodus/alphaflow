// Unit tests for alphaflow::market::ReplayLog and Logger.

#include <alphaflow/market/replay_log.hpp>

#include <alphaflow/concurrency/spsc_ring.hpp>
#include <alphaflow/market/logger.hpp>
#include <alphaflow/market/tick.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

using alphaflow::concurrency::SpscRing;
using alphaflow::market::FeedId;
using alphaflow::market::Logger;
using alphaflow::market::ReplayLog;
using alphaflow::market::Tick;

Tick make_tick(std::uint32_t id, double value, std::uint64_t sequence) {
    Tick tick;
    tick.id = id;
    tick.value = value;
    tick.seq = sequence;
    tick.feed = FeedId::A;
    tick.recv_tsc = 999;
    return tick;
}

}  // namespace

TEST_CASE("the replay log round-trips ticks", "[market][replay]") {
    const char* path = "replay_log_test.bin";

    ReplayLog log;
    REQUIRE(log.open(path));
    REQUIRE(log.valid());
    REQUIRE(log.append(make_tick(7, 0.05, 3)));
    REQUIRE(log.count() == 1);
    log.close();

    std::array<Tick, 8> out{};
    const std::size_t read = ReplayLog::read_all(path, out.data(), out.size());
    REQUIRE(read == 1);
    REQUIRE(out[0].id == 7);
    REQUIRE(out[0].value == 0.05);
    REQUIRE(out[0].seq == 3);
    REQUIRE(out[0].recv_tsc == 999);

    std::remove(path);
}

TEST_CASE("the logger drains the ring into the log", "[market][logger]") {
    const char* path = "logger_test.bin";

    ReplayLog log;
    REQUIRE(log.open(path));

    SpscRing<Tick, 16> ring;
    Logger<16> logger(ring, log);

    for (std::uint64_t i = 1; i <= 10; ++i) {
        REQUIRE(ring.try_push(make_tick(1, 0.03, i)));
    }

    REQUIRE(logger.drain_once() == 10);
    REQUIRE(logger.written() == 10);
    REQUIRE(log.count() == 10);
    log.close();

    std::remove(path);
}
