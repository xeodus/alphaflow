cmake_minimum_required(VERSION 3.25)

project(AlphaFlow
    VERSION 0.1.0
    DESCRIPTION "Real-time USD SOFR curve desk"
    LANGUAGES CXX
)

# --- Global settings --------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

option(ALPHAFLOW_BUILD_TESTS "Build unit/integration/oracle tests" ON)
option(ALPHAFLOW_BUILD_BENCHMARKS "Build microbenchmarks" ON)
option(ALPHAFLOW_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" OFF)
option(ALPHAFLOW_ENABLE_IPO "Enable interprocedural optimization (LTO)" OFF)

if(ALPHAFLOW_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _alphaflow_ipo_supported OUTPUT _alphaflow_ipo_message)
    if(_alphaflow_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "IPO requested but not supported: ${_alphaflow_ipo_message}")
    endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

# --- Dependencies -----------------------------------------------------------
if(ALPHAFLOW_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()

if(ALPHAFLOW_BUILD_BENCHMARKS)
    find_package(benchmark CONFIG REQUIRED)
endif()

find_package(hdr_histogram CONFIG REQUIRED)

# QuantLib is a TEST-ONLY oracle (ARCHITECTURE.md §13): never linked into the
# engine, enabled only by the dedicated `oracle` preset.
option(ALPHAFLOW_BUILD_ORACLE "Build the QuantLib oracle tests" OFF)
if(ALPHAFLOW_BUILD_ORACLE)
    find_package(QuantLib CONFIG REQUIRED)
endif()

# --- Targets ----------------------------------------------------------------
add_subdirectory(src)

if(ALPHAFLOW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
