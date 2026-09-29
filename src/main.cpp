#include <alphaflow/platform/clock.hpp>

#include <print>

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

    std::println("alphaflow — monotonic clock source: {}", source_name(Clock::source()));
    std::println("alphaflow — monotonic now: {} ns", Clock::now_ns());

    return 0;
}
