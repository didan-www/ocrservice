#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
readonly ROOT_DIR

fail() {
    printf 'run-qt-e2e: %s\n' "$1" >&2
    exit 1
}

[[ -n ${QT_E2E_EXECUTABLE:-} ]] || fail 'QT_E2E_EXECUTABLE is required'
[[ -n ${QT_E2E_QTMQTT_BIN:-} ]] || fail 'QT_E2E_QTMQTT_BIN is required'
[[ -n ${QT_E2E_QT_BIN:-} ]] || fail 'QT_E2E_QT_BIN is required'
[[ -n ${QT_E2E_MINGW_BIN:-} ]] || fail 'QT_E2E_MINGW_BIN is required'
[[ -n ${QT_E2E_USERNAME:-} ]] || fail 'QT_E2E_USERNAME is required'
[[ -n ${QT_E2E_PASSWORD:-} ]] || fail 'QT_E2E_PASSWORD is required'

if command -v cygpath >/dev/null 2>&1; then
    script_path=$(cygpath -w "$ROOT_DIR/tests/system/qt_e2e/run-qt-e2e.ps1")
elif command -v wslpath >/dev/null 2>&1; then
    script_path=$(wslpath -w "$ROOT_DIR/tests/system/qt_e2e/run-qt-e2e.ps1")
else
    script_path="$ROOT_DIR/tests/system/qt_e2e/run-qt-e2e.ps1"
fi

if command -v powershell.exe >/dev/null 2>&1; then
    powershell=(powershell.exe)
elif command -v pwsh >/dev/null 2>&1; then
    powershell=(pwsh)
else
    fail 'PowerShell is required'
fi

exec "${powershell[@]}" -NoProfile -ExecutionPolicy Bypass -File "$script_path" \
    -Executable "$QT_E2E_EXECUTABLE" \
    -QtMqttBin "$QT_E2E_QTMQTT_BIN" \
    -QtBin "$QT_E2E_QT_BIN" \
    -MingwBin "$QT_E2E_MINGW_BIN" "$@"
