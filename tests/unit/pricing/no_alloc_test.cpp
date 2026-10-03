// No-allocation tests for the RFQ path (ARCHITECTURE.md: nothing on the hot path
// allocates). The guard replaces the global allocator, so these tests are only
// compiled when no sanitizer is active.

#include "support/allocation_guard.hpp"

#include <alphaflow/concurrency/snapshot_ptr.hpp>
#include <alphaflow/core/date.hpp>
#include <alphaflow/curve/calendar.hpp>
#include <alphaflow/curve/curve_snapshot.hpp>
#include <alphaflow/curve/day_count.hpp>
#include <alphaflow/curve/schedule.hpp>
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
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>

#ifdef ALPHAFLOW_ALLOCATION_GUARD

namespace {

using alphaflow::concurrency::SnapshotPool;
using alphaflow::core::Date;
using alphaflow::curve::Calendar;
using alphaflow::curve::CurveSnapshot;
using alphaflow::curve::DayCount;
using alphaflow::curve::Frequency;
using alphaflow::curve::ScheduleSpec;
using alphaflow::metrics::LatencyRecorder;
using alphaflow::metrics::Stage;
using alphaflow::platform::Listener;
using alphaflow::platform::Socket;
using alphaflow::pricing::dv01;
using alphaflow::pricing::price_swap;
using alphaflow::pricing::SwapSpec;
using alphaflow::rfq::InstrumentUniverse;
using alphaflow::rfq::Request;
using alphaflow::rfq::Responder;
using alphaflow::rfq::Response;
using alphaflow::rfq::Server;
namespace protocol = alphaflow::rfq::protocol;

CurveSnapshot flat_snapshot() {
    CurveSnapshot snapshot;
    snapshot.generation = 1;
    snapshot.curve.reference = Date{2024, 1, 8};
    snapshot.curve.count = 6;
    for (std::size_t i = 0; i <= 5; ++i) {
        const Date d = Date{2024, 1, 8}.add_months(static_cast<int>(12 * i));
        const double t = snapshot.curve.time_of(d);
        snapshot.curve.times[i] = t;
        snapshot.curve.log_dfs[i] = -0.03 * t;
    }
    return snapshot;
}

SwapSpec five_year_swap() {
    ScheduleSpec spec;
    spec.effective = Date{2024, 1, 8};
    spec.termination = Date{2029, 1, 8};
    spec.frequency = Frequency::Annual;

    SwapSpec swap;
    swap.schedule = alphaflow::curve::make_schedule(spec, Calendar::united_states(),
                                                    DayCount::Actual360);
    swap.notional = 1'000'000.0;
    swap.fixed_rate = 0.03;
    return swap;
}

double stub_price(std::uint32_t, const CurveSnapshot& snapshot) noexcept {
    return snapshot.curve.discount(1.0);
}

std::atomic<bool> g_price_allocated{false};

// A volatile pointer forces the deliberate allocation below to be observable,
// so it survives -O3/LTO.
int* volatile g_self_test_leak = nullptr;

double guarded_price(std::uint32_t, const CurveSnapshot& snapshot) noexcept {
    alphaflow::test::begin_allocation_guard();
    const double price = price_swap(snapshot, five_year_swap()).par_rate;
    if (alphaflow::test::end_allocation_guard()) {
        g_price_allocated.store(true);
    }
    return price;
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
        std::array<std::byte, 128> buffer{};
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

TEST_CASE("the allocation guard detects an allocation", "[noalloc]") {
    alphaflow::test::begin_allocation_guard();
    g_self_test_leak = new int(5);
    delete g_self_test_leak;
    g_self_test_leak = nullptr;
    REQUIRE(alphaflow::test::end_allocation_guard());
}

TEST_CASE("the swap pricer allocates nothing", "[noalloc]") {
    const CurveSnapshot snapshot = flat_snapshot();
    const SwapSpec swap = five_year_swap();

    [[maybe_unused]] volatile double sink = 0.0;
    alphaflow::test::begin_allocation_guard();
    for (int i = 0; i < 1'000; ++i) {
        const auto quote = price_swap(snapshot, swap);
        sink = quote.pv + quote.par_rate;
    }
    REQUIRE_FALSE(alphaflow::test::end_allocation_guard());
}

TEST_CASE("DV01 allocates nothing", "[noalloc]") {
    const CurveSnapshot snapshot = flat_snapshot();
    const SwapSpec swap = five_year_swap();

    [[maybe_unused]] volatile double sink = 0.0;
    alphaflow::test::begin_allocation_guard();
    sink = dv01(snapshot, swap);
    REQUIRE_FALSE(alphaflow::test::end_allocation_guard());
}

TEST_CASE("the responder allocates nothing", "[noalloc]") {
    const CurveSnapshot snapshot = flat_snapshot();
    const Responder<CurveSnapshot> responder(InstrumentUniverse{4}, 1'000'000'000LL,
                                             &stub_price);

    alphaflow::test::begin_allocation_guard();
    const Response response = responder.respond(Request{1, 0}, &snapshot, 0);
    REQUIRE_FALSE(alphaflow::test::end_allocation_guard());
    REQUIRE(response.request_id == 1);
}

TEST_CASE("the protocol codec allocates nothing", "[noalloc]") {
    alphaflow::test::begin_allocation_guard();

    std::array<std::byte, 64> payload{};
    const auto payload_size = protocol::encode(Request{7, 2}, payload);
    std::array<std::byte, 128> wire{};
    const auto wire_size = protocol::frame(std::span(payload.data(), *payload_size), wire);
    protocol::FrameDecoder decoder;
    REQUIRE(decoder.feed(std::span(wire.data(), *wire_size)));
    std::span<const std::byte> frame;
    REQUIRE(decoder.next(frame) == protocol::FrameDecoder::Result::Frame);
    const auto request = protocol::decode_request(frame);

    REQUIRE_FALSE(alphaflow::test::end_allocation_guard());
    REQUIRE(request.has_value());
}

TEST_CASE("a snapshot lease allocates nothing", "[noalloc]") {
    SnapshotPool<CurveSnapshot, 4> pool;
    REQUIRE(pool.try_publish(flat_snapshot()));

    alphaflow::test::begin_allocation_guard();
    auto lease = pool.acquire();
    REQUIRE_FALSE(alphaflow::test::end_allocation_guard());
    REQUIRE(lease);
}

TEST_CASE("the live RFQ pipeline's pricing allocates nothing", "[noalloc]") {
    g_price_allocated.store(false);

    SnapshotPool<CurveSnapshot, 4> pool;
    REQUIRE(pool.try_publish(flat_snapshot()));
    const Responder<CurveSnapshot> responder(InstrumentUniverse{4}, 1'000'000'000LL,
                                             &guarded_price);
    LatencyRecorder latency(Stage::Rfq);

    auto listener = Listener::listen_loopback(0);
    REQUIRE(listener.has_value());

    std::thread server([&] {
        auto connection = listener->accept();
        if (!connection) {
            return;
        }
        Server<CurveSnapshot, 4> server(pool, responder, latency);
        server.serve(*connection);
    });

    auto client = Socket::connect_loopback(listener->port());
    REQUIRE(client.has_value());

    protocol::FrameDecoder decoder;
    for (std::uint32_t id = 0; id < 50; ++id) {
        REQUIRE(round_trip(*client, Request{id, id % 4}, decoder).has_value());
    }
    client->close();
    server.join();

    REQUIRE_FALSE(g_price_allocated.load());
}

#endif  // ALPHAFLOW_ALLOCATION_GUARD
