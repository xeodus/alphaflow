# Unit, integration, and oracle tests.
#
# The oracle target links QuantLib (test-only; gated by ALPHAFLOW_BUILD_ORACLE)
# and is never part of the engine.

add_executable(alphaflow_tests
    unit/platform/clock_test.cpp
    unit/platform/socket_test.cpp
    unit/core/date_test.cpp
    unit/curve/day_count_test.cpp
    unit/curve/calendar_test.cpp
    unit/curve/schedule_test.cpp
    unit/curve/discount_curve_test.cpp
    unit/curve/ois_instrument_test.cpp
    unit/curve/bootstrap_test.cpp
    unit/curve/futures_convexity_test.cpp
    unit/pricing/swap_pricer_test.cpp
    unit/market/sequence_arbiter_test.cpp
    unit/market/arbiter_test.cpp
    unit/market/synthetic_feed_test.cpp
    unit/market/replay_log_test.cpp
    unit/concurrency/spsc_ring_test.cpp
    unit/concurrency/seqlock_test.cpp
    unit/concurrency/latest_value_cache_test.cpp
    unit/concurrency/snapshot_pool_test.cpp
    unit/rfq/protocol_test.cpp
    unit/rfq/responder_test.cpp
    unit/rfq/server_test.cpp
    unit/metrics/histogram_test.cpp
    unit/metrics/interval_recorder_test.cpp
    unit/metrics/metrics_hub_test.cpp
    integration/skeleton_test.cpp
    integration/determinism_test.cpp
    integration/reliability_test.cpp
)

target_compile_features(alphaflow_tests PRIVATE cxx_std_23)
target_link_libraries(alphaflow_tests PRIVATE
    alphaflow::core
    Catch2::Catch2WithMain
)
alphaflow_enable_warnings(alphaflow_tests)
alphaflow_enable_sanitizers(alphaflow_tests)

include(Catch OPTIONAL)
if(COMMAND catch_discover_tests)
    catch_discover_tests(alphaflow_tests)
else()
    add_test(NAME alphaflow_tests COMMAND alphaflow_tests)
endif()

if(ALPHAFLOW_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()

# QuantLib oracle: an independent reference implementation to validate the curve
# and pricing against (ARCHITECTURE.md §13). Test-only; not sanitizer-built,
# since QuantLib itself is a prebuilt third-party library.
if(ALPHAFLOW_BUILD_ORACLE)
    add_executable(alphaflow_oracle_tests
        oracle/curve_oracle_test.cpp
    )
    target_compile_features(alphaflow_oracle_tests PRIVATE cxx_std_23)
    target_link_libraries(alphaflow_oracle_tests PRIVATE
        alphaflow::core
        QuantLib::QuantLib
        Catch2::Catch2WithMain
    )
    alphaflow_enable_warnings(alphaflow_oracle_tests)
    add_test(NAME alphaflow_oracle_tests COMMAND alphaflow_oracle_tests)
endif()
