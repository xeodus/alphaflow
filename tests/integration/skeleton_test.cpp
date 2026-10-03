// Integration test: the walking skeleton end to end on a real bootstrapped
// curve. Feed A/B -> arbiter -> QuoteCache -> CurveThread (bootstrap) -> pool ->
// RFQ server pricing real OIS swaps, with a client over loopback.

#include <alphaflow/concurrency/latest_value_cache.hpp>
#include <alphaflow/concurrency/snapshot_ptr.hpp>
#include <alphaflow/concurrency/spsc_ring.hpp>
#include <alphaflow/core/date.hpp>
#include <alphaflow/curve/bootstrap.hpp>
#include <alphaflow/curve/calendar.hpp>
#include <alphaflow/curve/curve_snapshot.hpp>
#include <alphaflow/curve/day_count.hpp>
#include <alphaflow/curve/schedule.hpp>
#include <alphaflow/market/arbiter.hpp>
#include <alphaflow/market/tick.hpp>
#include <alphaflow/metrics/histogram.hpp>
#include <alphaflow/platform/clock.hpp>
#include <alphaflow/platform/socket.hpp>
#include <alphaflow/pricing/swap_pricer.hpp>
#include <alphaflow/rfq/protocol.hpp>
#include <alphaflow/rfq/responder.hpp>
#include <alphaflow/rfq/server.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>

namespace {

using alphaflow::concurrency::LatestValueCache;
using alphaflow::concurrency::SnapshotPool;
using alphaflow::concurrency::SpscRing;
using alphaflow::core::Date;
using alphaflow::curve::BootstrapSpec;
using alphaflow::curve::Calendar;
using alphaflow::curve::CurveSnapshot;
using alphaflow::curve::DayCount;
using alphaflow::curve::Frequency;
using alphaflow::curve::OisPillar;
using alphaflow::curve::ScheduleSpec;
using alphaflow::market::Arbiter;
using alphaflow::market::FeedId;
using alphaflow::market::InstrumentId;
using alphaflow::market::Tick;
using alphaflow::metrics::LatencyRecorder;
using alphaflow::metrics::Stage;
using alphaflow::platform::Clock;
using alphaflow::platform::Listener;
using alphaflow::platform::Socket;
using alphaflow::pricing::price_swap;
using alphaflow::pricing::SwapSpec;
using alphaflow::rfq::InstrumentUniverse;
using alphaflow::rfq::Request;
using alphaflow::rfq::Responder;
using alphaflow::rfq::Response;
using alphaflow::rfq::Server;
using alphaflow::rfq::Status;
namespace protocol = alphaflow::rfq::protocol;

constexpr std::size_t kPoolDepth = 8;
constexpr std::size_t kInputCapacity = 256;
constexpr std::size_t kCacheCapacity = 32;
constexpr std::size_t kNotificationCapacity = 256;
constexpr std::size_t kPillars = 5;
constexpr std::uint32_t kInstrumentCount = static_cast<std::uint32_t>(kPillars);
constexpr std::uint32_t kRequestCount = 100;
constexpr alphaflow::platform::Nanos kStalenessThresholdNs = 1'000'000'000LL;
const Date kSpot{2024, 1, 8};

using InputRing = SpscRing<Tick, kInputCapacity>;
using NotificationRing = SpscRing<InstrumentId, kNotificationCapacity>;
using QuoteCache = LatestValueCache<double, kCacheCapacity>;
using ArbiterThread = Arbiter<kCacheCapacity, kNotificationCapacity, kInputCapacity>;

Date pillar_maturity(std::size_t index) {
    return kSpot.add_months(static_cast<int>(12 * (index + 1)));
}

const std::array<SwapSpec, kPillars>& book() {
    static const std::array<SwapSpec, kPillars> instances = [] {
        std::array<SwapSpec, kPillars> result{};
        for (std::size_t i = 0; i < kPillars; ++i) {
            ScheduleSpec spec;
            spec.effective = kSpot;
            spec.termination = pillar_maturity(i);
            spec.frequency = Frequency::Annual;
            result[i].schedule = alphaflow::curve::make_schedule(
                spec, Calendar::united_states(), DayCount::Actual360);
            result[i].notional = 1'000'000.0;
            result[i].fixed_rate = 0.03;
        }
        return result;
    }();
    return instances;
}

double real_price(std::uint32_t instrument_id, const CurveSnapshot& snapshot) noexcept {
    if (instrument_id >= kPillars) {
        return 0.0;
    }
    return price_swap(snapshot, book()[instrument_id]).par_rate;
}

void run_feed(InputRing& ring, FeedId feed, const std::atomic<bool>& stop) {
    std::uint64_t sequence = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        Tick tick;
        tick.id = static_cast<InstrumentId>(sequence % kPillars);
        tick.value = 0.03 + static_cast<double>(sequence % 5) * 0.001;
        tick.seq = ++sequence;
        tick.feed = feed;
        tick.recv_tsc = Clock::now_ticks();
        static_cast<void>(ring.try_push(tick));
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

std::optional<Response> round_trip(Socket& client, const Request& request,
                                   protocol::FrameDecoder& decoder) {
    std::array<std::byte, 64> payload{};
    const auto payload_size = protocol::encode(request, payload);
    if (!payload_size) {
        return std::nullopt;
    }
    std::array<std::byte, 128> wire{};
    const auto wire_size = protocol::frame(std::span(payload.data(), *payload_size), wire);
    if (!wire_size || !client.send_all(wire.data(), *wire_size)) {
        return std::nullopt;
    }

    for (;;) {
        std::span<const std::byte> frame;
        const auto result = decoder.next(frame);
        if (result == protocol::FrameDecoder::Result::Frame) {
            return protocol::decode_response(frame);
        }
        if (result == protocol::FrameDecoder::Result::Error) {
            return std::nullopt;
        }
        std::array<std::byte, 256> buffer{};
        const auto received = client.recv_some(buffer.data(), buffer.size());
        if (!received || *received == 0) {
            return std::nullopt;
        }
        if (!decoder.feed(std::span(buffer.data(), *received))) {
            return std::nullopt;
        }
    }
}

}  // namespace

TEST_CASE("the skeleton prices real swaps off a bootstrapped curve",
          "[integration][skeleton]") {
    InputRing ring_a;
    InputRing ring_b;
    QuoteCache cache;
    NotificationRing notifications;
    ArbiterThread arbiter(ring_a, ring_b, cache, notifications);
    SnapshotPool<CurveSnapshot, kPoolDepth> pool;

    std::atomic<bool> stop{false};
    LatencyRecorder rebuild_latency(Stage::Rebuild);
    LatencyRecorder rfq_latency(Stage::Rfq);

    const Calendar calendar = Calendar::united_states();
    BootstrapSpec bootstrap_spec;
    bootstrap_spec.reference = kSpot;
    bootstrap_spec.frequency = Frequency::Annual;

    // Publish an initial curve so the first request is not WarmingUp.
    {
        std::array<OisPillar, kPillars> pillars{};
        for (std::size_t i = 0; i < kPillars; ++i) {
            pillars[i] = OisPillar{pillar_maturity(i), 0.03};
        }
        CurveSnapshot initial;
        initial.generation = 1;
        initial.published_at_ns = Clock::now_ns();
        initial.curve = alphaflow::curve::bootstrap(pillars, calendar, bootstrap_spec).curve;
        REQUIRE(pool.try_publish(initial));
    }

    const Responder<CurveSnapshot> responder(InstrumentUniverse{kInstrumentCount},
                                             kStalenessThresholdNs, &real_price);

    auto listener = Listener::listen_loopback(0);
    REQUIRE(listener.has_value());
    const std::uint16_t port = listener->port();

    std::thread feed_a([&] { run_feed(ring_a, FeedId::A, stop); });
    std::thread feed_b([&] { run_feed(ring_b, FeedId::B, stop); });
    std::thread arbiter_thread([&] { arbiter.run(stop); });

    std::thread curve_thread([&] {
        std::array<double, kPillars> rates{};
        rates.fill(0.03);
        std::uint32_t generation = 1;
        while (!stop.load(std::memory_order_relaxed) || !notifications.empty()) {
            bool changed = false;
            InstrumentId id = 0;
            while (notifications.try_pop(id)) {
                if (id < kPillars) {
                    rates[id] = cache.load(id);
                    changed = true;
                }
            }
            if (!changed) {
                std::this_thread::yield();
                continue;
            }
            std::array<OisPillar, kPillars> pillars{};
            for (std::size_t i = 0; i < kPillars; ++i) {
                pillars[i] = OisPillar{pillar_maturity(i), rates[i]};
            }
            const auto start = Clock::now_ns();
            const auto boot = alphaflow::curve::bootstrap(pillars, calendar, bootstrap_spec);
            if (boot.converged) {
                CurveSnapshot snapshot;
                snapshot.generation = ++generation;
                snapshot.published_at_ns = Clock::now_ns();
                snapshot.curve = boot.curve;
                if (pool.try_publish(snapshot)) {
                    rebuild_latency.record_elapsed(start, Clock::now_ns());
                }
            }
        }
    });

    std::thread server_thread([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto connection = listener->accept_for(std::chrono::milliseconds(20));
            if (!connection) {
                continue;
            }
            Server<CurveSnapshot, kPoolDepth> server(pool, responder, rfq_latency);
            server.serve(*connection);
        }
    });

    auto client = Socket::connect_loopback(port);
    REQUIRE(client.has_value());

    protocol::FrameDecoder decoder;
    std::uint32_t ok_responses = 0;
    for (std::uint32_t id = 0; id < kRequestCount; ++id) {
        const auto response = round_trip(*client, Request{id, id % kInstrumentCount}, decoder);
        REQUIRE(response.has_value());
        REQUIRE(response->status == Status::Ok);
        REQUIRE(response->price > 0.0);
        ++ok_responses;
    }

    client->close();
    stop.store(true, std::memory_order_relaxed);
    feed_a.join();
    feed_b.join();
    arbiter_thread.join();
    curve_thread.join();
    server_thread.join();

    REQUIRE(ok_responses == kRequestCount);
    REQUIRE(rfq_latency.histogram().count() == kRequestCount);
    REQUIRE(rebuild_latency.histogram().count() >= 1);
    REQUIRE(arbiter.sequences().active() == FeedId::A);
}
