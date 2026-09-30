# Sanitizer configuration for AlphaFlow targets.
#
# Selected by the ALPHAFLOW_SANITIZER cache variable, which the CMake presets
# set (address / undefined / thread / address,undefined / none).
#
#   alphaflow_enable_sanitizers(<target>)

function(alphaflow_enable_sanitizers target)
    if(NOT ALPHAFLOW_SANITIZER OR ALPHAFLOW_SANITIZER STREQUAL "none")
        return()
    endif()

    if(ALPHAFLOW_SANITIZER STREQUAL "address")
        set(_sanitizer_flags -fsanitize=address)
    elseif(ALPHAFLOW_SANITIZER STREQUAL "undefined")
        set(_sanitizer_flags -fsanitize=undefined -fno-sanitize-recover=all)
    elseif(ALPHAFLOW_SANITIZER STREQUAL "thread")
        set(_sanitizer_flags -fsanitize=thread)
    elseif(ALPHAFLOW_SANITIZER STREQUAL "address,undefined")
        set(_sanitizer_flags -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
        message(FATAL_ERROR
            "Unknown ALPHAFLOW_SANITIZER='${ALPHAFLOW_SANITIZER}'. "
            "Expected one of: address, undefined, thread, address,undefined, none")
    endif()

    target_compile_options(${target} PRIVATE ${_sanitizer_flags} -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE ${_sanitizer_flags})

    # GCC's ThreadSanitizer cannot instrument atomic_thread_fence and warns
    # (-Wtsan) for the seqlock's fences (concurrency/seqlock.hpp). TSan models
    # relaxed atomics as synchronizing, so it never verified that ordering
    # anyway; the fence protocol is justified by the argument in the header and
    # by review. Suppress the known limitation so it cannot turn into an error
    # once a first-party header is compiled into a -Werror target. Clang does
    # not emit this warning, hence the GNU guard.
    if(ALPHAFLOW_SANITIZER STREQUAL "thread" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE -Wno-tsan)
    endif()
endfunction()
