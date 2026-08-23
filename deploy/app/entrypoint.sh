#!/usr/bin/env bash
set -Eeuo pipefail
umask 027

readonly APP_USER=ocrservice
readonly MODEL_ROOT=/app/models
readonly IMAGE_ROOT=${IMAGE_ROOT:-/app/data/images}
readonly LOG_ROOT=${LOG_ROOT:-/app/data/logs}

fail() {
    printf 'ocrservice-entrypoint: startup failed (%s)\n' "$1" >&2
    exit 1
}

[[ $# -gt 0 ]] || fail 'missing command'
[[ -x "$1" ]] || fail 'command is not executable'
[[ -f "$MODEL_ROOT/yolov8_plate.onnx" && -f "$MODEL_ROOT/lprnet.onnx" ]] || fail 'model files missing'
printf '%s  %s\n' bfa426b74d4b619207cca55b296ce3603fb784755a205791af0d0dab4fcf6c04 "$MODEL_ROOT/yolov8_plate.onnx" | sha256sum -c - >/dev/null || fail 'YOLO model hash mismatch'
printf '%s  %s\n' c78e54070d0e2b8a6f8d548feb52b75321d5f464bcaadb679ec7ee31433cffa4 "$MODEL_ROOT/lprnet.onnx" | sha256sum -c - >/dev/null || fail 'LPR model hash mismatch'

mkdir -p "$IMAGE_ROOT" "$LOG_ROOT"
chown -R "$APP_USER:$APP_USER" "$IMAGE_ROOT" "$LOG_ROOT" 2>/dev/null || true

if [[ $(id -u) -eq 0 ]]; then
    exec gosu "$APP_USER" "$@"
fi
exec "$@"
