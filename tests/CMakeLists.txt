# Unit, integration, and oracle tests.
#
# Oracle tests depend on the (test-only) QuantLib oracle feature and are added
# in a later milestone; this target currently hosts unit and integration tests.

add_executable(alphaflow_tests
    unit/platform/clock_test.cpp
    unit/platform/socket_test.cpp
    unit/core/date_test.cpp
    unit/curve/day_count_test.cpp
    unit/curve/calendar_test.cpp
    unit/market/sequence_arbiter_test.cpp
    unit/market/arbiter_test.cpp
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
