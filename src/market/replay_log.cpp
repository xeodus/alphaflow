#include <alphaflow/market/replay_log.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>

namespace alphaflow::market {

ReplayLog::~ReplayLog() {
    close();
}

bool ReplayLog::open(const char* path) noexcept {
    close();
    fd_ = ::open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    count_ = 0;
    return fd_ >= 0;
}

bool ReplayLog::valid() const noexcept {
    return fd_ >= 0;
}

bool ReplayLog::append(const Tick& tick) noexcept {
    if (fd_ < 0) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(&tick);
    std::size_t written = 0;
    while (written < sizeof(Tick)) {
        const ssize_t n = ::write(fd_, bytes + written, sizeof(Tick) - written);
        if (n <= 0) {
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    ++count_;
    return true;
}

void ReplayLog::close() noexcept {
    if (fd_ >= 0) {
        static_cast<void>(::close(fd_));
        fd_ = -1;
    }
}

std::size_t ReplayLog::read_all(const char* path, Tick* out, std::size_t capacity) noexcept {
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    std::size_t count = 0;
    while (count < capacity) {
        const ssize_t n = ::read(fd, out + count, sizeof(Tick));
        if (n != static_cast<ssize_t>(sizeof(Tick))) {
            break;
        }
        ++count;
    }
    static_cast<void>(::close(fd));
    return count;
}

}  // namespace alphaflow::market
