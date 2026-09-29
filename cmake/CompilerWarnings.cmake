# Warning configuration for AlphaFlow targets.
#
# Usage:
#   alphaflow_enable_warnings(<target>)              # enable the warning set
#   alphaflow_enable_warnings_as_errors(<target>)    # add -Werror / /WX
#
# Warnings-as-errors is applied only to first-party targets. Third-party
# headers (Catch2, benchmark) must never fail the build, so tests and
# benchmarks opt in to warnings but not to -Werror by default.

function(alphaflow_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wcast-align
            -Wformat=2
            -Wundef
            -Wold-style-cast
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wdouble-promotion
            -Wnull-dereference
            -Wimplicit-fallthrough
        )
    endif()
endfunction()

function(alphaflow_enable_warnings_as_errors target)
    if(NOT ALPHAFLOW_WARNINGS_AS_ERRORS)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /WX)
    else()
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()
