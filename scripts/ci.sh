#!/usr/bin/env bash
#
# Local mirror of the continuous-integration matrix.
#
# Builds and tests every preset the GitHub workflow exercises, so that
# configuration and portability mistakes surface before a push. It runs on
# macOS and Linux, but it cannot reproduce CI's Linux GCC toolchain from macOS:
# treat it as a pre-flight check, not a substitute for CI.
#
# The CI baseline is pinned to ubuntu-24.04 (GCC 13 / libstdc++ 13). Avoid
# standard-library features newer than that baseline -- <print> is GCC 14+.
#
# Usage:
#   scripts/ci.sh              # all CI presets
#   scripts/ci.sh dev asan     # a subset

set -u -o pipefail

cd "$(dirname "$0")/.." || exit 2

if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake not found in PATH" >&2
    exit 2
fi

if [ ! -f "${HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake" ]; then
    echo "error: vcpkg not found at \$HOME/vcpkg (see README.md)" >&2
    exit 2
fi

if [ "$#" -gt 0 ]; then
    presets=("$@")
else
    presets=(dev release asan ubsan tsan)
fi

failures=()
log_dir="build/ci-logs"
mkdir -p "${log_dir}"

for preset in "${presets[@]}"; do
    log="${log_dir}/${preset}.log"
    echo "==> ${preset}"
    if {
        cmake --preset "${preset}" \
            && cmake --build --preset "${preset}" \
            && ctest --preset "${preset}" --output-on-failure \
            && "./build/${preset}/src/alpha"
    } >"${log}" 2>&1; then
        echo "    ok"
    else
        echo "    FAILED (log: ${log})" >&2
        tail -n 40 "${log}" >&2
        failures+=("${preset}")
    fi
done

if [ "${#failures[@]}" -ne 0 ]; then
    echo "FAILED presets: ${failures[*]}" >&2
    exit 1
fi

echo "all presets passed"
