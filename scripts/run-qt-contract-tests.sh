#!/usr/bin/env bash
set -euo pipefail

if (($# > 1)); then
    echo "usage: $0 [build-directory]" >&2
    exit 2
fi

readonly SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
readonly BUILD_DIRECTORY="${1:-${SOURCE_ROOT}/build}"

if [[ ! -f "${BUILD_DIRECTORY}/CTestTestfile.cmake" ]]; then
    echo "Qt contract build directory is not configured: ${BUILD_DIRECTORY}" >&2
    exit 2
fi

# The Qt client budgets JSON queries at 10 s, images at 30 s and CSV at 120 s.
# The contract binary enforces those per-request budgets; this outer limit catches hangs.
timeout --signal=TERM --kill-after=5s 150s \
    ctest --test-dir "${BUILD_DIRECTORY}" \
        -R '^contract\.qt\.' \
        --output-on-failure
