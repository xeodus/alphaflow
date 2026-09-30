# AlphaFlow library and executable.
#
# The core library is the engine's stateless, testable surface. Platform/
# abstractions are compiled into it; the executable only wires threads and
# configuration together.

add_library(alphaflow_core STATIC
    platform/clock.cpp
    platform/socket.cpp
    rfq/protocol.cpp
)
add_library(alphaflow::core ALIAS alphaflow_core)

target_include_directories(alphaflow_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(alphaflow_core PUBLIC cxx_std_23)
alphaflow_enable_warnings(alphaflow_core)
alphaflow_enable_warnings_as_errors(alphaflow_core)
alphaflow_enable_sanitizers(alphaflow_core)

add_executable(alpha main.cpp)
target_link_libraries(alpha PRIVATE alphaflow::core)
alphaflow_enable_warnings(alpha)
alphaflow_enable_warnings_as_errors(alpha)
alphaflow_enable_sanitizers(alpha)
