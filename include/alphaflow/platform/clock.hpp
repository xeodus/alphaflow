#pragma once

#include <cstdint>

namespace alphaflow::platform {

/// Raw hardware counter unit (TSC ticks on x86 Linux, mach absolute time on
/// macOS, nanoseconds on platforms without either).
using Ticks = std::uint64_t;

/// Nanoseconds on the monotonic timeline.
using Nanos = std::int64_t;

/// Which hardware counter backs the clock on this machine. Reported so that
/// latency reports can state the time source, and so tests can assert that a
/// known clock was selected.
enum class ClockSource : std::uint8_t {
    MachAbsoluteTime,  ///< macOS `mach_absolute_time`.
    InvariantTsc,      ///< x86 `RDTSC`, verified invariant via CPUID.
    MonotonicRaw,      ///< `CLOCK_MONOTONIC_RAW` (or a portable fallback).
};

/// Monotonic clock for latency measurement.
///
/// This clock is strictly non-decreasing and is never adjusted for wall-clock
/// drift, so differences between two readings are always valid elapsed time.
/// Wall-clock (business-date) time is a separate concern and lives in
/// `core/time`; the two are deliberately never mixed.
///
/// Calibration (including invariant-TSC detection and measurement) happens once
/// on first use and is idempotent. All methods are safe to call from multiple
/// threads.
class Clock {
public:
    Clock() = delete;

    /// Force calibration to happen now. Idempotent and thread-safe.
    static void initialize() noexcept;

    /// True once calibration has completed (explicitly or on first use).
    [[nodiscard]] static bool is_initialized() noexcept;

    /// The hardware counter backing this clock.
    [[nodiscard]] static ClockSource source() noexcept;

    /// Read the raw monotonic counter. Non-decreasing.
    [[nodiscard]] static Ticks now_ticks() noexcept;

    /// Convert a raw counter reading to nanoseconds. Monotonic in `ticks`.
    [[nodiscard]] static Nanos ticks_to_ns(Ticks ticks) noexcept;

    /// Current time on the monotonic timeline, in nanoseconds.
    [[nodiscard]] static Nanos now_ns() noexcept;
};

}  // namespace alphaflow::platform
