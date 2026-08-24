# Embedded simulator system test

Run from the repository root after building `embedded_device_simulator`:

```bash
EMBEDDED_SIMULATOR_BIN=build-task022-dev/tests/tests/simulators_embedded/embedded_device_simulator \
  scripts/run-embedded-e2e.sh
```

The runner creates an isolated Compose project and removes its containers, volumes, credentials,
summaries, MQTT payload fixtures, and generated images when it exits. It invokes the production
`scripts/provision-device.sh`; the simulator receives only that script's 0600 JSON summary.

The default success candidate is stored as the ASCII fixture
`fixtures/vehicle-1363-q75.jpg.b64` and is decoded to JPEG only in the temporary test directory.
Its source is `yolov8-tensorrt/runs/detect/predict/1363.png` in the Apache-2.0 model source
repository recorded in `models/LICENSES.md` (source PNG SHA-256
`efd640803d8beaac7bdad6b04792022bf48d1df51d1b201824042c2368a20dcf`). The fixture was
deterministically converted with Pillow to RGB JPEG using quality 75, optimize enabled, and
progressive disabled; its decoded SHA-256 is
`c3cefbfe4685ba8c05a61b63b163cbc832846a8c341c8232ac85f01c0f122126`.

An alternate PNG or JPEG must be supplied with its explicit SHA-256 through
`EMBEDDED_E2E_SUCCESS_IMAGE_SHA256`.
