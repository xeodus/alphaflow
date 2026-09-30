#include <alphaflow/platform/clock.hpp>

#include <iostream>

// Note: this file deliberately uses <iostream> rather than C++23 <print>.
// <print>/std::println are library features that require libstdc++ >= 14 or
// libc++ >= 18; the CI baseline is GCC 13 / libstdc++ 13, which does not ship
// them. The engine's value is in the library and tests, so the demo executable
// stays on the most portable facilities available.

namespace {

using alphaflow::platform::Clock;
using alphaflow::platform::ClockSource;

const char* source_name(ClockSource source) {
    switch (source) {
        case ClockSource::MachAbsoluteTime:
            return "mach_absolute_time";
        case ClockSource::InvariantTsc:
            return "invariant TSC";
        case ClockSource::MonotonicRaw:
            return "CLOCK_MONOTONIC_RAW";
    }
    return "unknown";
}

}  // namespace

int main() {
    Clock::initialize();

    std::cout << "alphaflow - monotonic clock source: " << source_name(Clock::source())
              << '\n';
    std::cout << "alphaflow - monotonic now: " << Clock::now_ns() << " ns\n";

    return 0;
}
