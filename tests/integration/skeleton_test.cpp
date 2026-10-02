// Integration test: the walking skeleton, end to end, now through the arbiter.
//
// Feed A/B -> Arbiter -> QuoteCache + notifications -> CurveThread -> pool ->
// RFQ server, with a client over loopback. Checks that requests are answered Ok
// from a published snapshot, that the socket-to-socket span is recorded, and
// that the arbiter drove the cache. Runs under ASan/UBSan/TSan.

#include <alphaflow/concurrency/latest_value_cache.hpp>
#include <alphaflow/concurrency/snapshot_ptr.hpp>
#include <alphaflow/concurrency/spsc_ring.hpp>
#include <alphaflow/curve/curve_snapshot.hpp>
#include <alphaflow/market/arbiter.hpp>
#include <alphaflow/market/tick.hpp>
#include <alphaflow/metrics/histogram.hpp>
#include <alphaflow/platform/clock.hpp>
#include <alphaflow/platform/socket.hpp>
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
using alphaflow::curve::CurveSnapshot;
using alphaflow::market::Arbiter;
using alphaflow::market::FeedId;
using alphaflow::market::InstrumentId;
using alphaflow::market::Tick;
using alphaflow::metrics::LatencyRecorder;
using alphaflow::metrics::Stage;
using alphaflow::platform::Clock;
using alphaflow::platform::Listener;
using alphaflow::platform::Socket;
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
constexpr std::uint32_t kInstrumentCount = 4;
constexpr std::uint32_t kRequestCount = 100;
constexpr alphaflow::platform::Nanos kStalenessThresholdNs = 1'000'000'000LL;

using InputRing = SpscRing<Tick, kInputCapacity>;
using NotificationRing = SpscRing<InstrumentId, kNotificationCapacity>;
using QuoteCache = LatestValueCache<double, kCacheCapacity>;
using ArbiterThread = Arbiter<kCacheCapacity, kNotificationCapacity, kInputCapacity>;

double stub_price(std::uint32_t instrument_id, const CurveSnapshot& snapshot) noexcept {
    return snapshot.par_rate * (1.0 + static_cast<double>(instrument_id));
}

void run_feed(InputRing& ring, FeedId feed, const std::atomic<bool>& stop) {
    std::uint64_t sequence = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        Tick tick;
        tick.id = static_cast<InstrumentId>(sequence % kInstrumentCount);
        tick.value = 0.02 + static_cast<double>(sequence % 7) * 0.001;
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

TEST_CASE("the walking skeleton answers RFQs end to end through the arbiter",
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

    REQUIRE(pool.try_publish(CurveSnapshot{1, Clock::now_ns(), 0.02}));

    const Responder<CurveSnapshot> responder(InstrumentUniverse{kInstrumentCount},
                                             kStalenessThresholdNs, &stub_price);

    auto listener = Listener::listen_loopback(0);
    REQUIRE(listener.has_value());
    const std::uint16_t port = listener->port();

    std::thread feed_a([&] { run_feed(ring_a, FeedId::A, stop); });
    std::thread feed_b([&] { run_feed(ring_b, FeedId::B, stop); });
    std::thread arbiter_thread([&] { arbiter.run(stop); });

    std::thread curve_thread([&] {
        std::array<double, kInstrumentCount> rates{};
        std::uint32_t generation = 1;
        while (!stop.load(std::memory_order_relaxed) || !notifications.empty()) {
            bool changed = false;
            InstrumentId id = 0;
            while (notifications.try_pop(id)) {
                if (id < kInstrumentCount) {
                    rates[id] = cache.load(id);
                    changed = true;
                }
            }
            if (!changed) {
                std::this_thread::yield();
                continue;
            }
            double sum = 0.0;
            for (double rate : rates) {
                sum += rate;
            }
            const auto start = Clock::now_ns();
            ++generation;
            if (pool.try_publish(
                    CurveSnapshot{generation, Clock::now_ns(), sum / kInstrumentCount})) {
                rebuild_latency.record_elapsed(start, Clock::now_ns());
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
    std::uint32_t max_generation = 0;
    for (std::uint32_t id = 0; id < kRequestCount; ++id) {
        const auto response = round_trip(*client, Request{id, id % kInstrumentCount}, decoder);
        REQUIRE(response.has_value());
        REQUIRE(response->status == Status::Ok);
        ++ok_responses;
        if (response->generation > max_generation) {
            max_generation = response->generation;
        }
    }

    client->close();
    stop.store(true, std::memory_order_relaxed);
    feed_a.join();
    feed_b.join();
    arbiter_thread.join();
    curve_thread.join();
    server_thread.join();

    REQUIRE(ok_responses == kRequestCount);
    REQUIRE(max_generation >= 1);
    REQUIRE(rfq_latency.histogram().count() == kRequestCount);
    REQUIRE(rebuild_latency.histogram().count() >= 1);
    REQUIRE(arbiter.sequences().duplicate_count() == 0);
    REQUIRE(arbiter.sequences().active() == FeedId::A);
}
