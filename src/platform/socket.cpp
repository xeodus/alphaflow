#include <alphaflow/platform/socket.hpp>

#include <cerrno>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace alphaflow::platform {
namespace {

void suppress_sigpipe(int fd) noexcept {
#if defined(SO_NOSIGPIPE)
    // macOS/BSD: per-socket option.
    int enabled = 1;
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)));
#else
    static_cast<void>(fd);
#endif
}

int send_flags() noexcept {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;  // Linux: per-call flag.
#else
    return 0;
#endif
}

}  // namespace

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

Socket::~Socket() {
    close();
}

bool Socket::valid() const noexcept {
    return fd_ >= 0;
}

void Socket::close() noexcept {
    if (fd_ >= 0) {
        static_cast<void>(::close(fd_));
        fd_ = -1;
    }
}

std::optional<Socket> Socket::connect_loopback(std::uint16_t port) noexcept {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return std::nullopt;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        static_cast<void>(::close(fd));
        return std::nullopt;
    }

    suppress_sigpipe(fd);
    return Socket{fd};
}

bool Socket::send_all(const std::byte* data, std::size_t size) noexcept {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t written = ::send(fd_, data + sent, size - sent, send_flags());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

std::optional<std::size_t> Socket::recv_some(std::byte* data, std::size_t size) noexcept {
    for (;;) {
        const ssize_t received = ::recv(fd_, data, size, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::nullopt;
        }
        return static_cast<std::size_t>(received);
    }
}

Listener::Listener(Listener&& other) noexcept : fd_(other.fd_), port_(other.port_) {
    other.fd_ = -1;
    other.port_ = 0;
}

Listener& Listener::operator=(Listener&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        port_ = other.port_;
        other.fd_ = -1;
        other.port_ = 0;
    }
    return *this;
}

Listener::~Listener() {
    close();
}

void Listener::close() noexcept {
    if (fd_ >= 0) {
        static_cast<void>(::close(fd_));
        fd_ = -1;
    }
}

std::optional<Listener> Listener::listen_loopback(std::uint16_t port, int backlog) noexcept {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return std::nullopt;
    }

    int reuse = 1;
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        static_cast<void>(::close(fd));
        return std::nullopt;
    }
    if (::listen(fd, backlog) != 0) {
        static_cast<void>(::close(fd));
        return std::nullopt;
    }

    sockaddr_in bound{};
    socklen_t bound_length = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
        static_cast<void>(::close(fd));
        return std::nullopt;
    }

    Listener listener;
    listener.fd_ = fd;
    listener.port_ = ntohs(bound.sin_port);
    return listener;
}

std::optional<Socket> Listener::accept() noexcept {
    for (;;) {
        const int fd = ::accept(fd_, nullptr, nullptr);
        if (fd >= 0) {
            suppress_sigpipe(fd);
            return Socket{fd};
        }
        if (errno == EINTR) {
            continue;
        }
        return std::nullopt;
    }
}

std::optional<Socket> Listener::accept_for(std::chrono::milliseconds timeout) noexcept {
    pollfd descriptor{};
    descriptor.fd = fd_;
    descriptor.events = POLLIN;

    const int ready = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    if (ready <= 0) {
        return std::nullopt;  // timeout (0) or error (< 0)
    }
    return accept();
}

}  // namespace alphaflow::platform
