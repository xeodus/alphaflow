// AlphaFlow walking skeleton.
//
// The full M1 data path in miniature, wired through every primitive:
//
//   feed A ──SPSC──┐
//   feed B ──SPSC──┴─▶ ArbiterThread ──▶ QuoteCache + notification ring
//                                             │
//                                             ▼
//                                       CurveThread ──▶ SnapshotPool
//                                                             │
//                        self-load client ──▶ RFQ server ─────┘
//
// The feeds are synthetic and the pricer is a stub; what is real is the
// lock-free ring, the single-writer arbiter, the per-instrument quote cache,
// the immutable-snapshot publication, the socket-to-socket RFQ path, and its
// measurement. Real market data, curve, and pricing land in later phases.
//
// Usage: alpha [seconds]     (default 5)

#include <alphaflow/concurrency/latest_value_cache.hpp>
#include <alphaflow/concurrency/snapshot_ptr.hpp>
#include <alphaflow/concurrency/spsc_ring.hpp>
#include <alphaflow/curve/curve_snapshot.hpp>
#include <alphaflow/market/arbiter.hpp>
#include <alphaflow/market/tick.hpp>
#include <alphaflow/metrics/histogram.hpp>
#include <alphaflow/metrics/interval_recorder.hpp>
#include <alphaflow/metrics/metrics_hub.hpp>
#include <alphaflow/platform/clock.hpp>
#include <alphaflow/platform/socket.hpp>
#include <alphaflow/rfq/protocol.hpp>
#include <alphaflow/rfq/responder.hpp>
#include <alphaflow/rfq/server.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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
using alphaflow::metrics::LatencyProfile;
using alphaflow::metrics::LatencyRecorder;
using alphaflow::metrics::MetricsHub;
using alphaflow::metrics::IntervalRecorder;
using alphaflow::metrics::Stage;
using alphaflow::platform::Clock;
using alphaflow::platform::Listener;
using alphaflow::platform::Nanos;
using alphaflow::platform::Socket;
using alphaflow::rfq::InstrumentUniverse;
using alphaflow::rfq::Request;
using alphaflow::rfq::Responder;
using alphaflow::rfq::Server;
namespace protocol = alphaflow::rfq::protocol;

constexpr std::size_t kPoolDepth = 8;
constexpr std::size_t kInputCapacity = 1024;
constexpr std::size_t kCacheCapacity = 64;
constexpr std::size_t kNotificationCapacity = 1024;
constexpr std::uint32_t kInstrumentCount = 8;
constexpr Nanos kStalenessThresholdNs = 100'000'000;  // 100 ms

using InputRing = SpscRing<Tick, kInputCapacity>;
using NotificationRing = SpscRing<InstrumentId, kNotificationCapacity>;
using QuoteCache = LatestValueCache<double, kCacheCapacity>;
using ArbiterThread = Arbiter<kCacheCapacity, kNotificationCapacity, kInputCapacity>;

double stub_price(std::uint32_t instrument_id, const CurveSnapshot& snapshot) noexcept {
    return snapshot.par_rate * (1.0 + static_cast<double>(instrument_id));
}

void print_profile(const LatencyProfile& profile) {
    std::cout << "  " << alphaflow::metrics::stage_name(profile.stage)
              << ": n=" << profile.count << " min=" << profile.min_ns
              << "ns p50=" << profile.p50_ns << "ns p90=" << profile.p90_ns
              << "ns p99=" << profile.p99_ns << "ns p99.9=" << profile.p999_ns
              << "ns max=" << profile.max_ns << "ns\n";
    std::cout << "    span: " << alphaflow::metrics::stage_span(profile.stage) << "\n";
}

/// A deterministic synthetic feed line: a fixed LCG random walk, one tick at a
/// time, cycling through the instruments. (The full replayable generator is a
/// later milestone; this is enough to exercise the data path.)
void run_feed(InputRing& ring, FeedId feed, std::uint64_t seed,
              const std::atomic<bool>& stop) {
    std::uint64_t state = seed;
    std::uint64_t sequence = 0;
    double value = 0.02;

    while (!stop.load(std::memory_order_relaxed)) {
        state = state * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
        const double noise =
            (static_cast<double>((state >> 40) & 0xFFFFU) / 65'535.0 - 0.5) * 0.001;
        value += noise;

        Tick tick;
        tick.id = static_cast<InstrumentId>(sequence % kInstrumentCount);
        tick.value = value;
        tick.seq = ++sequence;
        tick.feed = feed;
        tick.recv_tsc = Clock::now_ticks();
        static_cast<void>(ring.try_push(tick));  // full: drop (coalesced upstream)

        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

/// A synchronous self-load client: send a request, wait for its response.
void run_client(std::uint16_t port, std::atomic<bool>& stop,
                std::atomic<std::uint64_t>& responses) {
    auto client = Socket::connect_loopback(port);
    if (!client) {
        return;
    }

    protocol::FrameDecoder decoder;
    std::uint32_t request_id = 0;

    while (!stop.load(std::memory_order_relaxed)) {
        const Request request{request_id, request_id % kInstrumentCount};
        ++request_id;

        std::array<std::byte, 64> payload{};
        const auto payload_size = protocol::encode(request, payload);
        if (!payload_size) {
            break;
        }
        std::array<std::byte, 128> wire{};
        const auto wire_size = protocol::frame(std::span(payload.data(), *payload_size), wire);
        if (!wire_size || !client->send_all(wire.data(), *wire_size)) {
            break;
        }

        bool answered = false;
        while (!answered) {
            std::span<const std::byte> frame;
            const auto result = decoder.next(frame);
            if (result == protocol::FrameDecoder::Result::Frame) {
                if (protocol::decode_response(frame)) {
                    responses.fetch_add(1, std::memory_order_relaxed);
                }
                answered = true;
                continue;
            }
            if (result == protocol::FrameDecoder::Result::Error) {
                return;
            }
            std::array<std::byte, 256> buffer{};
            const auto received = client->recv_some(buffer.data(), buffer.size());
            if (!received || *received == 0) {
                return;
            }
            if (!decoder.feed(std::span(buffer.data(), *received))) {
                return;
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 5;

    InputRing ring_a;
    InputRing ring_b;
    QuoteCache cache;
    NotificationRing notifications;
    ArbiterThread arbiter(ring_a, ring_b, cache, notifications);
    SnapshotPool<CurveSnapshot, kPoolDepth> pool;

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> responses{0};
    std::atomic<std::uint64_t> republished{0};

    IntervalRecorder rebuild_recorder;
    IntervalRecorder rfq_recorder;
    MetricsHub metrics_hub;
    static_cast<void>(metrics_hub.add(Stage::Rebuild, rebuild_recorder));
    static_cast<void>(metrics_hub.add(Stage::Rfq, rfq_recorder));

    const Responder<CurveSnapshot> responder(InstrumentUniverse{kInstrumentCount},
                                             kStalenessThresholdNs, &stub_price);

    auto listener = Listener::listen_loopback(0);
    if (!listener) {
        std::cerr << "error: could not bind a loopback port\n";
        return 1;
    }
    const std::uint16_t port = listener->port();
    std::cout << "alphaflow skeleton on 127.0.0.1:" << port << " for " << seconds << "s\n";

    std::thread feed_a([&] { run_feed(ring_a, FeedId::A, 0x9e37'79b9'7f4a'7c15ULL, stop); });
    std::thread feed_b([&] { run_feed(ring_b, FeedId::B, 0xbf58'476d'1ce4'e5b9ULL, stop); });

    std::thread arbiter_thread([&] { arbiter.run(stop); });

    std::thread curve_thread([&] {
        std::array<double, kInstrumentCount> rates{};
        std::uint32_t generation = 0;
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
                rebuild_recorder.record_elapsed(start, Clock::now_ns());
                republished.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::thread server_thread([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto connection = listener->accept_for(std::chrono::milliseconds(20));
            if (!connection) {
                continue;
            }
            Server<CurveSnapshot, kPoolDepth, IntervalRecorder> server(pool, responder,
                                                                      rfq_recorder);
            server.serve(*connection);
        }
    });

    std::thread client_thread([&] { run_client(port, stop, responses); });

    // The live Metrics thread drains the recorders while they record.
    std::thread metrics_thread(
        [&] { metrics_hub.run(stop, std::chrono::milliseconds(100)); });

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop.store(true, std::memory_order_relaxed);

    client_thread.join();
    feed_a.join();
    feed_b.join();
    arbiter_thread.join();
    curve_thread.join();
    server_thread.join();
    metrics_thread.join();
    metrics_hub.sample_once();  // final drain after all recorders stopped

    std::cout << "responses: " << responses.load(std::memory_order_relaxed)
              << "  snapshots republished: " << republished.load(std::memory_order_relaxed)
              << "\n";
    std::cout << "arbiter: active=" << (arbiter.sequences().active() == FeedId::A ? "A" : "B")
              << " duplicates=" << arbiter.sequences().duplicate_count()
              << " gaps(A)=" << arbiter.sequences().gap_count(FeedId::A)
              << " gaps(B)=" << arbiter.sequences().gap_count(FeedId::B) << "\n";
    std::cout << "metrics:\n";
    print_profile(metrics_hub.profile(Stage::Rebuild));
    print_profile(metrics_hub.profile(Stage::Rfq));
    return 0;
}
