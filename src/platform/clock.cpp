#include <alphaflow/platform/clock.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#elif defined(__linux__)
#include <ctime>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <x86intrin.h>
#endif
#endif

namespace alphaflow::platform {
namespace {

struct Calibration {
    ClockSource source{ClockSource::MonotonicRaw};
    double ns_per_tick{1.0};
};

std::atomic<bool> g_initialized{false};

#if defined(__APPLE__)

Calibration calibrate() noexcept {
    mach_timebase_info_data_t timebase{};
    static_cast<void>(mach_timebase_info(&timebase));

    Calibration calibration;
    calibration.source = ClockSource::MachAbsoluteTime;
    calibration.ns_per_tick =
        static_cast<double>(timebase.numer) / static_cast<double>(timebase.denom);
    return calibration;
}

Ticks read_counter(ClockSource) noexcept {
    return static_cast<Ticks>(mach_absolute_time());
}

#elif defined(__linux__)

Nanos monotonic_raw_ns() noexcept {
    timespec ts{};
    static_cast<void>(clock_gettime(CLOCK_MONOTONIC_RAW, &ts));
    return static_cast<Nanos>(ts.tv_sec) * 1'000'000'000LL +
           static_cast<Nanos>(ts.tv_nsec);
}

#if defined(__x86_64__) || defined(__i386__)

/// True if CPUID reports a nonstop (invariant) TSC: CPUID.80000007H:EDX[8].
/// Without this guarantee the TSC frequency can change with power state and
/// must not be used for elapsed-time measurement.
bool has_invariant_tsc() noexcept {
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;

    if (__get_cpuid_max(0x80000000u, nullptr) < 0x80000007u) {
        return false;
    }
    __cpuid(0x80000007u, eax, ebx, ecx, edx);

    constexpr unsigned int kNonstopTsc = 1u << 8;
    return (edx & kNonstopTsc) != 0;
}

/// Measure nanoseconds per TSC tick against CLOCK_MONOTONIC_RAW. Several
/// samples are taken and the one spanning the most ticks is kept, as it is
/// least perturbed by scheduling.
Calibration calibrate() noexcept {
    Calibration calibration;
    if (!has_invariant_tsc()) {
        calibration.source = ClockSource::MonotonicRaw;
        calibration.ns_per_tick = 1.0;
        return calibration;
    }

    double best_ns_per_tick = 0.0;
    Ticks best_delta = 0;

    for (int sample = 0; sample < 5; ++sample) {
        const Nanos ns_before = monotonic_raw_ns();
        const Ticks ticks_before = static_cast<Ticks>(__rdtsc());
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        const Ticks ticks_after = static_cast<Ticks>(__rdtsc());
        const Nanos ns_after = monotonic_raw_ns();

        if (ns_after <= ns_before) {
            continue;
        }
        const Ticks delta = ticks_after - ticks_before;
        if (delta > best_delta) {
            best_delta = delta;
            best_ns_per_tick =
                static_cast<double>(ns_after - ns_before) / static_cast<double>(delta);
        }
    }

    if (best_delta == 0 || best_ns_per_tick <= 0.0) {
        calibration.source = ClockSource::MonotonicRaw;
        calibration.ns_per_tick = 1.0;
        return calibration;
    }

    calibration.source = ClockSource::InvariantTsc;
    calibration.ns_per_tick = best_ns_per_tick;
    return calibration;
}

Ticks read_counter(ClockSource source) noexcept {
    if (source == ClockSource::InvariantTsc) {
        return static_cast<Ticks>(__rdtsc());
    }
    return static_cast<Ticks>(monotonic_raw_ns());
}

#else  // __linux__ on a non-x86 architecture (e.g. aarch64)

Calibration calibrate() noexcept {
    Calibration calibration;
    calibration.source = ClockSource::MonotonicRaw;
    calibration.ns_per_tick = 1.0;
    return calibration;
}

Ticks read_counter(ClockSource) noexcept {
    return static_cast<Ticks>(monotonic_raw_ns());
}

#endif  // x86

#else  // Other platforms: portable fallback, no TSC.

Calibration calibrate() noexcept {
    Calibration calibration;
    calibration.source = ClockSource::MonotonicRaw;
    calibration.ns_per_tick = 1.0;
    return calibration;
}

Ticks read_counter(ClockSource) noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<Ticks>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

#endif

/// Calibrate once, thread-safely, on first use.
const Calibration& calibration() noexcept {
    static const Calibration value = [] {
        const Calibration result = calibrate();
        g_initialized.store(true, std::memory_order_release);
        return result;
    }();
    return value;
}

Nanos to_ns(const Calibration& calibration, Ticks ticks) noexcept {
    const double nanoseconds = static_cast<double>(ticks) * calibration.ns_per_tick;
    return static_cast<Nanos>(nanoseconds + 0.5);
}

}  // namespace

void Clock::initialize() noexcept {
    static_cast<void>(calibration());
}

bool Clock::is_initialized() noexcept {
    return g_initialized.load(std::memory_order_acquire);
}

ClockSource Clock::source() noexcept {
    return calibration().source;
}

Ticks Clock::now_ticks() noexcept {
    return read_counter(calibration().source);
}

Nanos Clock::ticks_to_ns(Ticks ticks) noexcept {
    return to_ns(calibration(), ticks);
}

Nanos Clock::now_ns() noexcept {
    const Calibration& current = calibration();
    return to_ns(current, read_counter(current.source));
}

}  // namespace alphaflow::platform
