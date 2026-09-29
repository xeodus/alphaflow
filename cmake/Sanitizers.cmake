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
endfunction()
