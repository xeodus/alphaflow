# Unit, integration, and oracle tests.
#
# Oracle tests depend on the (test-only) QuantLib oracle feature and are added
# in a later milestone; this target currently hosts unit and integration tests.

add_executable(alphaflow_tests
    unit/platform/clock_test.cpp
    unit/concurrency/spsc_ring_test.cpp
    unit/concurrency/seqlock_test.cpp
    unit/concurrency/latest_value_cache_test.cpp
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
