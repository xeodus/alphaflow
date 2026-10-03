#pragma once

#include <alphaflow/market/tick.hpp>

#include <cstddef>

namespace alphaflow::market {

/// Append-only replay log of accepted ticks (ADR-015).
///
/// A fixed-size record per tick, appended by the Logger thread so the ingest and
/// arbitration threads never touch the disk. It is the input side of the
/// determinism contract: replaying the same log reproduces the same state.
class ReplayLog {
public:
    ReplayLog() noexcept = default;
    ~ReplayLog();

    ReplayLog(const ReplayLog&) = delete;
    ReplayLog& operator=(const ReplayLog&) = delete;

    /// Create (truncate) the log at `path` for writing.
    [[nodiscard]] bool open(const char* path) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool append(const Tick& tick) noexcept;
    void close() noexcept;

    [[nodiscard]] std::size_t count() const noexcept { return count_; }

    /// Read up to `capacity` records into `out`; returns the number read.
    [[nodiscard]] static std::size_t read_all(const char* path, Tick* out,
                                              std::size_t capacity) noexcept;

private:
    int fd_{-1};
    std::size_t count_{0};
};

}  // namespace alphaflow::market
