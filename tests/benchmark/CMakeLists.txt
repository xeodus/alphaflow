# Microbenchmarks (Google Benchmark).
#
# These measure the cost of individual operations (clock reads, pricers,
# serialization). End-to-end request latency is measured by the engine's own
# histograms, not here. Kept under tests/ per ARCHITECTURE.md §15.

add_executable(alphaflow_benchmarks
    platform/clock_benchmark.cpp
    pricing/swap_pricer_benchmark.cpp
)

target_compile_features(alphaflow_benchmarks PRIVATE cxx_std_23)
target_link_libraries(alphaflow_benchmarks PRIVATE
    alphaflow::core
    benchmark::benchmark
)
alphaflow_enable_warnings(alphaflow_benchmarks)
alphaflow_enable_sanitizers(alphaflow_benchmarks)
