// AlphaFlow walking skeleton.
//
// This wires the Phase 1/2 primitives into a running system -- the tracer
// bullet that proves the plumbing before the real market data, curve, and
// pricing land in Phase 3:
//
//   synthetic producer -> SPSC ring -> curve thread -> SnapshotPool
//                                                          |
//                          self-load client -> RFQ server <-+
//
// The "curve" is the placeholder CurveSnapshot; the producer and pricer are
// stand-ins. What is real: the lock-free ring, the immutable-snapshot
// publication, the socket-to-socket RFQ path, and the measurement of it.
//
// Usage: alpha [seconds]     (default 5)

#include <alphaflow/concurrency/snapshot_ptr.hpp>
#include <alphaflow/concurrency/spsc_ring.hpp>
#include <alphaflow/curve/curve_snapshot.hpp>
#include <alphaflow/metrics/histogram.hpp>
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

using alphaflow::concurrency::SnapshotPool;
using alphaflow::concurrency::SpscRing;
using alphaflow::curve::CurveSnapshot;
using alphaflow::metrics::LatencyProfile;
using alphaflow::metrics::LatencyRecorder;
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
constexpr std::size_t kRingCapacity = 1024;
constexpr std::uint32_t kInstrumentCount = 8;
constexpr Nanos kStalenessThresholdNs = 100'000'000;  // 100 ms

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

/// A synchronous self-load client: send a request, wait for its response.
void run_client(std::uint16_t port, std::uint32_t instrument_count, std::atomic<bool>& stop,
                std::atomic<std::uint64_t>& responses) {
    auto client = Socket::connect_loopback(port);
    if (!client) {
        return;
    }

    protocol::FrameDecoder decoder;
    std::uint32_t request_id = 0;

    while (!stop.load(std::memory_order_relaxed)) {
        const Request request{request_id, request_id % instrument_count};
        ++request_id;

        std::array<std::byte, 64> payload{};
        const auto payload_size = protocol::encode(request, payload);
        if (!payload_size) {
            break;
        }
        std::array<std::byte, 128> wire{};
        const auto wire_size =
            protocol::frame(std::span(payload.data(), *payload_size), wire);
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

    SnapshotPool<CurveSnapshot, kPoolDepth> pool;
    SpscRing<double, kRingCapacity> ring;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> responses{0};

    LatencyRecorder rebuild_latency(Stage::Rebuild);
    LatencyRecorder rfq_latency(Stage::Rfq);

    const Responder<CurveSnapshot> responder(InstrumentUniverse{kInstrumentCount},
                                             kStalenessThresholdNs, &stub_price);

    auto listener = Listener::listen_loopback(0);
    if (!listener) {
        std::cerr << "error: could not bind a loopback port\n";
        return 1;
    }
    const std::uint16_t port = listener->port();
    std::cout << "alphaflow skeleton on 127.0.0.1:" << port << " for " << seconds
              << "s\n";

    std::thread producer([&] {
        double rate = 0.02;
        while (!stop.load(std::memory_order_relaxed)) {
            static_cast<void>(ring.try_push(rate));  // ring full: drop (coalesce upstream)
            rate += 0.0001;
            if (rate > 0.05) {
                rate = 0.02;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::thread curve([&] {
        std::uint32_t generation = 0;
        while (!stop.load(std::memory_order_relaxed) || !ring.empty()) {
            double update = 0.0;
            if (!ring.try_pop(update)) {
                std::this_thread::yield();
                continue;
            }
            const Nanos start = Clock::now_ns();
            ++generation;
            if (pool.try_publish(CurveSnapshot{generation, Clock::now_ns(), update})) {
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

    std::thread client_thread([&] { run_client(port, kInstrumentCount, stop, responses); });

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop.store(true, std::memory_order_relaxed);

    client_thread.join();
    producer.join();
    curve.join();
    server_thread.join();

    std::cout << "responses: " << responses.load(std::memory_order_relaxed) << "\n";
    std::cout << "metrics:\n";
    print_profile(rebuild_latency.profile());
    print_profile(rfq_latency.profile());
    return 0;
}
