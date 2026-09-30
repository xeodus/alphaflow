// Unit tests for alphaflow::platform::Socket / Listener.
//
// The socket layer is the only place the engine talks to the OS network stack
// (ARCHITECTURE.md §12). For M1 it is ordinary blocking TCP over loopback; the
// busy-poll / SO_REUSEPORT hardening is a later milestone. These tests pin the
// behaviour the RFQ server will rely on: bind, connect, accept, send, receive,
// detect peer close, and clean up on destruction.

#include <alphaflow/platform/socket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

namespace {

using alphaflow::platform::Listener;
using alphaflow::platform::Socket;

}  // namespace

TEST_CASE("a listener binds an ephemeral loopback port", "[platform][socket]") {
    auto listener = Listener::listen_loopback(0);
    REQUIRE(listener.has_value());
    REQUIRE(listener->valid());
    REQUIRE(listener->port() != 0);
}

TEST_CASE("connecting to a port with no listener fails", "[platform][socket]") {
    std::uint16_t port = 0;
    {
        auto listener = Listener::listen_loopback(0);
        REQUIRE(listener.has_value());
        port = listener->port();
    }  // listener closed here

    auto socket = Socket::connect_loopback(port);
    REQUIRE_FALSE(socket.has_value());
}

TEST_CASE("a socket is invalid until connected", "[platform][socket]") {
    Socket socket;
    REQUIRE_FALSE(socket.valid());

    auto listener = Listener::listen_loopback(0);
    REQUIRE(listener.has_value());
    auto connected = Socket::connect_loopback(listener->port());
    REQUIRE(connected.has_value());
    REQUIRE(connected->valid());
}

TEST_CASE("loopback round-trips bytes between client and server",
          "[platform][socket]") {
    auto listener = Listener::listen_loopback(0);
    REQUIRE(listener.has_value());
    const std::uint16_t port = listener->port();

    const std::string message = "alphaflow round-trip";

    std::string server_received;
    std::atomic<bool> server_ok{false};

    std::thread server([&] {
        auto connection = listener->accept();
        if (!connection) {
            return;
        }
        std::array<std::byte, 128> buffer{};
        const auto received = connection->recv_some(buffer.data(), buffer.size());
        if (!received || *received == 0) {
            return;
        }
        if (!connection->send_all(buffer.data(), *received)) {
            return;
        }
        server_received.assign(reinterpret_cast<const char*>(buffer.data()), *received);
        server_ok.store(true);
    });

    auto client = Socket::connect_loopback(port);
    REQUIRE(client.has_value());
    REQUIRE(client->send_all(reinterpret_cast<const std::byte*>(message.data()),
                            message.size()));

    std::array<std::byte, 128> reply{};
    const auto reply_size = client->recv_some(reply.data(), reply.size());

    server.join();

    REQUIRE(server_ok.load());
    REQUIRE(server_received == message);
    REQUIRE(reply_size.has_value());
    REQUIRE(std::string(reinterpret_cast<const char*>(reply.data()), *reply_size) == message);
}

TEST_CASE("recv reports zero when the peer closes", "[platform][socket]") {
    auto listener = Listener::listen_loopback(0);
    REQUIRE(listener.has_value());

    std::atomic<bool> saw_close{false};

    std::thread server([&] {
        auto connection = listener->accept();
        if (!connection) {
            return;
        }
        std::array<std::byte, 16> buffer{};
        const auto received = connection->recv_some(buffer.data(), buffer.size());
        if (received.has_value() && *received == 0) {
            saw_close.store(true);
        }
    });

    auto client = Socket::connect_loopback(listener->port());
    REQUIRE(client.has_value());
    client->close();

    server.join();
    REQUIRE(saw_close.load());
}

TEST_CASE("sockets are movable", "[platform][socket]") {
    auto listener = Listener::listen_loopback(0);
    REQUIRE(listener.has_value());

    auto client = Socket::connect_loopback(listener->port());
    REQUIRE(client.has_value());

    Socket moved = std::move(*client);
    REQUIRE(moved.valid());
    REQUIRE_FALSE(client->valid());

    auto connection = listener->accept();
    REQUIRE(connection.has_value());
}
