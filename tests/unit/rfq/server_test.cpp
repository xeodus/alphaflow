// Unit tests for alphaflow::rfq::Server over a real loopback socket.
//
// These exercise the I/O half of the RFQ path: framing incoming bytes,
// dispatching each request through the responder against a leased snapshot,
// framing and sending the response, and recording the socket-to-socket span.

#include <alphaflow/rfq/server.hpp>

#include <alphaflow/concurrency/snapshot_ptr.hpp>
#include <alphaflow/metrics/histogram.hpp>
#include <alphaflow/platform/clock.hpp>
#include <alphaflow/platform/socket.hpp>
#include <alphaflow/rfq/protocol.hpp>
#include <alphaflow/rfq/responder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>

namespace {

using alphaflow::concurrency::SnapshotPool;
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

/// A minimal snapshot for exercising the server without the real curve.
struct StubSnapshot {
    std::uint32_t generation{0};
    alphaflow::platform::Nanos published_at_ns{0};
    double par_rate{0.0};
};

double stub_price(std::uint32_t instrument_id, const StubSnapshot& snapshot) noexcept {
    return snapshot.par_rate * (1.0 + static_cast<double>(instrument_id));
}

std::optional<Response> send_and_receive(Socket& client, const Request& request,
                                         protocol::FrameDecoder& decoder) {
    std::array<std::byte, 64> payload{};
    const auto payload_size = protocol::encode(request, payload);
    if (!payload_size) {
        return std::nullopt;
    }
    std::array<std::byte, 128> wire{};
    const auto frame_size = protocol::frame(std::span(payload.data(), *payload_size), wire);
    if (!frame_size || !client.send_all(wire.data(), *frame_size)) {
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
        std::array<std::byte, 64> buffer{};
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

TEST_CASE("the server answers a request from the current snapshot",
          "[rfq][server]") {
    SnapshotPool<StubSnapshot, 4> pool;
    REQUIRE(pool.try_publish(StubSnapshot{1, Clock::now_ns(), 0.02}));

    const Responder<StubSnapshot> responder(InstrumentUniverse{4}, 1'000'000'000LL, &stub_price);
    LatencyRecorder latency(Stage::Rfq);

    auto listener = Listener::listen_loopback(0);
    REQUIRE(listener.has_value());

    std::atomic<bool> server_ok{false};
    std::thread server_thread([&] {
        auto connection = listener->accept();
        if (!connection) {
            return;
        }
        Server<StubSnapshot, 4> server(pool, responder, latency);
        server.serve(*connection);
        server_ok.store(true);
    });

    auto client = Socket::connect_loopback(listener->port());
    REQUIRE(client.has_value());

    protocol::FrameDecoder decoder;
    const auto response = send_and_receive(*client, Request{11, 2}, decoder);
    client->close();
    server_thread.join();

    REQUIRE(server_ok.load());
    REQUIRE(response.has_value());
    REQUIRE(response->request_id == 11);
    REQUIRE(response->status == Status::Ok);
    REQUIRE(response->generation == 1);
    REQUIRE(response->price == 0.02 * 3.0);

    // The socket-to-socket span was recorded exactly once.
    REQUIRE(latency.histogram().count() == 1);
}

TEST_CASE("the server answers several framed requests in one connection",
          "[rfq][server]") {
    SnapshotPool<StubSnapshot, 4> pool;
    REQUIRE(pool.try_publish(StubSnapshot{9, Clock::now_ns(), 0.05}));

    const Responder<StubSnapshot> responder(InstrumentUniverse{2}, 1'000'000'000LL, &stub_price);
    LatencyRecorder latency(Stage::Rfq);

    auto listener = Listener::listen_loopback(0);
    REQUIRE(listener.has_value());

    std::thread server_thread([&] {
        auto connection = listener->accept();
        if (!connection) {
            return;
        }
        Server<StubSnapshot, 4> server(pool, responder, latency);
        server.serve(*connection);
    });

    auto client = Socket::connect_loopback(listener->port());
    REQUIRE(client.has_value());

    protocol::FrameDecoder decoder;
    const auto first = send_and_receive(*client, Request{1, 0}, decoder);
    const auto second = send_and_receive(*client, Request{2, 1}, decoder);
    client->close();
    server_thread.join();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->request_id == 1);
    REQUIRE(second->request_id == 2);
    REQUIRE(first->status == Status::Ok);
    REQUIRE(second->status == Status::Ok);
    REQUIRE(second->price == 0.05 * 2.0);
    REQUIRE(latency.histogram().count() == 2);
}

TEST_CASE("the server reports warming up when nothing is published",
          "[rfq][server]") {
    SnapshotPool<StubSnapshot, 4> pool;  // deliberately empty

    const Responder<StubSnapshot> responder(InstrumentUniverse{2}, 1'000'000'000LL, &stub_price);
    LatencyRecorder latency(Stage::Rfq);

    auto listener = Listener::listen_loopback(0);
    REQUIRE(listener.has_value());

    std::thread server_thread([&] {
        auto connection = listener->accept();
        if (!connection) {
            return;
        }
        Server<StubSnapshot, 4> server(pool, responder, latency);
        server.serve(*connection);
    });

    auto client = Socket::connect_loopback(listener->port());
    REQUIRE(client.has_value());

    protocol::FrameDecoder decoder;
    const auto response = send_and_receive(*client, Request{1, 0}, decoder);
    client->close();
    server_thread.join();

    REQUIRE(response.has_value());
    REQUIRE(response->status == Status::WarmingUp);
    REQUIRE(latency.histogram().count() == 1);
}
