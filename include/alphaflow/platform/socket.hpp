#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace alphaflow::platform {

/// A connected byte-stream socket. Move-only; closes on destruction.
///
/// This is deliberately minimal: it exists so that engine code never includes
/// `<sys/socket.h>` or equivalent (ARCHITECTURE.md §12). The RFQ server speaks
/// a length-prefixed binary protocol over it; nothing above this layer knows
/// whether the transport is a kernel socket, a loopback, or (later) a
/// kernel-bypass NIC.
class Socket {
public:
    Socket() noexcept = default;
    ~Socket();

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] bool valid() const noexcept;

    /// Connect to 127.0.0.1:port. Returns std::nullopt on failure.
    [[nodiscard]] static std::optional<Socket> connect_loopback(std::uint16_t port) noexcept;

    /// Send all `size` bytes, blocking until done. Returns false on error.
    [[nodiscard]] bool send_all(const std::byte* data, std::size_t size) noexcept;

    /// Receive up to `size` bytes. Returns the count transferred (0 means the
    /// peer closed the connection), or std::nullopt on error.
    [[nodiscard]] std::optional<std::size_t> recv_some(std::byte* data, std::size_t size) noexcept;

    void close() noexcept;

private:
    explicit Socket(int fd) noexcept : fd_(fd) {}

    int fd_{-1};

    friend class Listener;
};

/// A listening socket bound to the loopback interface. Move-only.
class Listener {
public:
    Listener() noexcept = default;
    ~Listener();

    Listener(Listener&& other) noexcept;
    Listener& operator=(Listener&& other) noexcept;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    /// Bind to 127.0.0.1:port (port 0 selects an ephemeral port) and listen.
    /// Returns std::nullopt on failure.
    [[nodiscard]] static std::optional<Listener> listen_loopback(
        std::uint16_t port, int backlog = 64) noexcept;

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    /// The port actually bound (useful when port 0 was requested).
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    /// Block until a connection arrives. Returns std::nullopt on error.
    [[nodiscard]] std::optional<Socket> accept() noexcept;

    void close() noexcept;

private:
    int fd_{-1};
    std::uint16_t port_{0};
};

}  // namespace alphaflow::platform
