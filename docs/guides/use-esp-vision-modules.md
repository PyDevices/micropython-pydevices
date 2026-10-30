# Use esp-vision's modules

Two modules come from [espressif/esp-vision](https://github.com/espressif/esp-vision)
(Apache-2.0; its imlib is OpenMV's, MIT), compiled from esp-vision's own
files. Both are ESP32-P4 and ESP32-S3 only, both need ulab, and both are
opt-in: `--modules all` leaves them out, so name them:

```bash
./build_mp.py --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI --flash 16MB --modules ulab,tflite,vision
```

On a P4 before chip revision 3, build the `PRE_REV3_C6_WIFI` variant instead.
`h264` works there too, but that chip's encoder reads only YUV420, so each
frame is converted in software first: 30 frames at 320x240 took 635 ms to
encode on a revision 1.3 chip, against 263 ms on a revision 3 one.

- **`tflite`**: TensorFlow Lite Micro inference (`tflite.Model`), the API
  OpenMV's `ml` module uses.
- **`vision`**: the OpenMV-style `image` API (`image`, `imageio`), ESP-DL
  inference (`espdl`), and on the P4 the hardware H.264 encoder (`h264`) and an
  RTSP server (`rtsp`).

esp-vision's stubs (`stubs/*.pyi` in esp-vision) document the API. The desktop
ports skip both modules, and they build for MicroPython only.

Built into an `all` image they add about 2.5 MB on the S3 and 3 MB on the P4,
most of it ESP-DL, so check [that the image fits](fit-firmware-on-a-small-flash.md).

`image.flush()` raises `OSError("preview flush failed: ESP_ERR_NOT_SUPPORTED")`,
because esp-vision's preview streams frames to its own IDE and this build has no
such connection.

## Streaming models

`tflite` also runs streaming (stateful) models, such as the microWakeWord
models ESPHome publishes. They keep their state between calls in resource
variables, and each `Model` has room for 20. Feed one a few slices at a time
and read its output after each `predict()`.

Their input is already int8, so fill the tensor with a callable rather than an
ndarray, which `predict()` would quantize again. The callable gets the input
tensor's own memory, its shape and its dtype:

```python
group = bytearray(stride * 40)  # the next stride slices, int8


def fill(buffer, shape, dtype):
    buffer[:] = group


probability = model.predict([fill])[0][0][0]  # uint8: 255 is certain
```

[`tflite_streaming_simpletest.py`](../../modules/tflite/examples/tflite_streaming_simpletest.py)
runs one, and
[`tflite_wakeword.py`](../../modules/tflite/examples/tflite_wakeword.py)
listens for a wake word, with audiodsp's `audiofeatures` making the slices
and ESPHome's detection rules in Python.
[`tflite_wakeword_mic.py`](../../modules/tflite/examples/tflite_wakeword_mic.py)
does the same live from the board's microphone. It reads the microphone with
`board_peripherals.pcm_in()` and hands each block to the frontend's `feed()`:
an audiopump `Tap` sees only what the board plays, so it can't carry a
microphone.

## Models that run

These ran on an ESP32-P4 board, each with an example. The arena is what
`Model.ram` reports, and the time is one `predict()`.

| Model | Example | Arena | Time |
|---|---|---|---|
| microWakeWord hey_jarvis | [`tflite_wakeword_mic.py`](../../modules/tflite/examples/tflite_wakeword_mic.py) | 19,728 bytes | 1.8 ms |
| micro_speech (yes, no) | [`tflite_micro_speech.py`](../../modules/tflite/examples/tflite_micro_speech.py) | 7,840 bytes | 6 ms |
| yamnet-cry-distill-int8 (521 sounds) | [`tflite_sounds.py`](../../modules/tflite/examples/tflite_sounds.py) | 128,128 bytes | 28 ms |
| person_detection | [`tflite_person_detect.py`](../../modules/tflite/examples/tflite_person_detect.py) | 102,416 bytes | 50 ms |
| MobileNet-v2 int8 (ImageNet) | [`tflite_mobilenet.py`](../../modules/tflite/examples/tflite_mobilenet.py) | 2,608,176 bytes | 1.5 s |

Each example's docstring says where to download its model.

## Run long scripts as `main.py`

An mpftp `exec` that outlasts mpftp's wait resets the board over USB
(`rst:0x17 CHIP_USB_UART_RESET`). Under a long-running script that looks like
the firmware crashing. Run such scripts as `main.py`, or with nothing attached.

How the modules are built into the firmware is in
[How esp-vision's modules are built](../how-it-works/esp-vision.md).
