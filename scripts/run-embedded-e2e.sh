#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec "$ROOT_DIR/tests/system/embedded_e2e/test_embedded_e2e.sh" "$@"
