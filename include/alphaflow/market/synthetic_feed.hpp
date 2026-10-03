#pragma once

#include <alphaflow/market/tick.hpp>
#include <alphaflow/platform/clock.hpp>

#include <cstdint>

namespace alphaflow::market {

/// Deterministic synthetic quote generator (ARCHITECTURE.md §6, criterion 1).
///
/// Uses a fixed LCG — never `std::random`, whose output is implementation-defined
/// — so the same seed yields the same tick content (id, value, sequence) on any
/// platform. That content determinism is what the replay log and the determinism
/// test rely on; `recv_tsc` is the only non-deterministic field and is captured
/// (and later replayed) from the clock.
class SyntheticFeed {
public:
    SyntheticFeed(FeedId feed, std::uint32_t instrument_count, double base_rate,
                  std::uint64_t seed) noexcept
        : feed_(feed),
          instrument_count_(instrument_count),
          value_(base_rate),
          state_(seed) {}

    [[nodiscard]] Tick next() noexcept {
        state_ = state_ * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
        const double noise =
            (static_cast<double>((state_ >> 40) & 0xFFFFU) / 65'535.0 - 0.5) * 0.001;
        value_ += noise;
        if (value_ < 0.001) {
            value_ = 0.001;
        }
        if (value_ > 0.20) {
            value_ = 0.20;
        }

        Tick tick;
        tick.id = static_cast<InstrumentId>(sequence_ % instrument_count_);
        tick.value = value_;
        tick.seq = ++sequence_;
        tick.feed = feed_;
        tick.recv_tsc = platform::Clock::now_ticks();
        return tick;
    }

    [[nodiscard]] std::uint64_t produced() const noexcept { return sequence_; }

private:
    FeedId feed_;
    std::uint32_t instrument_count_;
    double value_;
    std::uint64_t state_;
    std::uint64_t sequence_{0};
};

}  // namespace alphaflow::market
