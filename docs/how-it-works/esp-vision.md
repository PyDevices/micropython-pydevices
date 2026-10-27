# How esp-vision's modules are built

[Use esp-vision's modules](../guides/use-esp-vision-modules.md) says how to
build with `tflite` and `vision`. This page says what is behind them.

## The pin and the glue

[`modules/.esp-vision`](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/.esp-vision)
pins esp-vision at a full commit in `ESP_VISION.lock`. Its `fetch.sh` links a
sibling clone at that commit (`../esp-vision`), or fetches that commit alone.
The leading dot keeps it out of `--modules` and `all`, because esp-vision's own
`micropython.cmake` is its whole firmware build, not a module.

[`modules/tflite`](../../modules/tflite/) and [`modules/vision`](../../modules/vision/) are our glue around esp-vision's files:
the ESP-IDF components esp-vision uses, at its versions; the include paths and
definitions MicroPython's QSTR scan needs (it sees only a module's own paths,
never a linked component's); and two pieces of our own.

- **imlib compiles inside the module**, not as an ESP-IDF component. As a
  component it would wait for MicroPython's generated headers while being one of
  their inputs (a ninja dependency cycle). esp-vision's own `components/imlib`
  CMakeLists can't be used anyway: it finds its per-board headers by
  `MICROPY_BOARD`, which names upstream's generic boards here. Ours takes them
  from one of esp-vision's boards per chip (`ESP32_P4X_FUNCTION_EV_BOARD`,
  `ESP32_S3_EYE`).
- **`src/vision_preview_none.c`** stands in for esp-vision's preview, which
  streams frames to its IDE over its own USB multiplexer.

Two build details follow from esp-vision's code. The prebuilt RTSP library calls
`ESP_ERROR_CHECK`'s handler, which nothing else references with MicroPython's
assertions off, so `vision` links it with `-u _esp_error_check_failed`, the way
the port does for `abort_`. And ESP-DL's headers are C++20, so the module's C++
compiles with `-std=gnu++2b`, esp-vision's own setting.

## What was checked on hardware

| Test | P4 DEV-KIT | S3, octal PSRAM |
|---|---|---|
| `tflite`: TFLM's own hello-world test (its int8 model, four golden inputs, 0.05 tolerance) | pass, worst error 0.016 | pass, outputs identical |
| `tflite`: esp-vision's sine model, per prediction | 104 µs | 397 µs |
| `vision`: [`examples/vision_smoke.py`](https://github.com/PyDevices/micropython-pydevices/blob/main/modules/vision/examples/vision_smoke.py) (draw, read back, `find_blobs`, JPEG out and in, `flush`) | pass; JPEG 42 ms | pass; JPEG 84 ms |
| `espdl`: ESP-DL's pedestrian example photo, against ESP-DL's published boxes | pass, 68 ms, IoU ≥ 0.90 | pass, 176 ms, IoU ≥ 0.92 |
| `h264`: 30 frames decoded on the PC by ffmpeg | Constrained Baseline, frames as drawn | (P4 only) |
| `rtsp`: ffmpeg on the PC pulls the stream | 151 frames in 10 s at 15 fps | (P4 only) |

Each test fails when it should: the hello-world test with its inputs offset, the
pedestrian test with one person blacked out.

## What it costs

Built into an `all` image, the two modules added about 2.5 MB on the S3 (the
LCD-7's app partition grew from 0x470000 to 0x6d0000, leaving about 1 MB of
filesystem on 8 MB of flash) and about 3.1 MB on the P4 (`firmware.bin` 4.6 MiB
to 7.6 MiB of 16 MB). Most of it is ESP-DL. That is why they are opt-in.

## Not included

- **`sensor`** (the camera). esp-vision's camera code is per board, and every P4
  board's `camera.c` uses Espressif's Board Manager (its YAML board definitions).
  The other route: esp-vision's generic `platform/camera.c` declares the camera
  functions weak, so our glue could provide them from cameraif, which already
  drives the OV5647.
- **`display`**. esp-vision's display code also needs Board Manager, and
  displayif already does displays here.
- **One `esp_h264`.** h264enc pins 1.4.1 and `vision` takes the same; esp-vision
  uses 1.3.0. `sensor` would bring `esp_video`, which requires 1.3.*, so a
  firmware with h264enc and a camera via `esp_video` doesn't resolve.
