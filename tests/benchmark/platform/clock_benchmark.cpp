// Microbenchmarks for the platform clock.
//
// These establish the cost of the two reads every latency measurement depends
// on. They are not performance claims; they are regression guards and inputs
// to the latency budget.

#include <alphaflow/platform/clock.hpp>

#include <benchmark/benchmark.h>

namespace {

namespace platform = alphaflow::platform;

void BM_ClockNowNs(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(platform::Clock::now_ns());
    }
}
BENCHMARK(BM_ClockNowNs);

void BM_ClockNowTicks(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(platform::Clock::now_ticks());
    }
}
BENCHMARK(BM_ClockNowTicks);

}  // namespace

BENCHMARK_MAIN();
