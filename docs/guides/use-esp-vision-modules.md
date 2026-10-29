# Use esp-vision's modules

Two modules come from [espressif/esp-vision](https://github.com/espressif/esp-vision)
(Apache-2.0; its imlib is OpenMV's, MIT), compiled from esp-vision's own files
without changes. Both are ESP32-P4 and ESP32-S3 only, both need ulab, and both
are opt-in: `--modules all` leaves them out, so name them:

```bash
./build_mp.py --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI --flash 16MB --modules ulab,tflite,vision
```

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

[`tflite_streaming_simpletest.py`](https://github.com/PyDevices/micropython-pydevices/blob/main/modules/tflite/examples/tflite_streaming_simpletest.py)
runs one, and
[`tflite_wakeword.py`](https://github.com/PyDevices/micropython-pydevices/blob/main/modules/tflite/examples/tflite_wakeword.py)
listens for a wake word, with audiodsp's `audiofeatures` making the slices
and ESPHome's detection rules in Python.

## Run long scripts as `main.py`

An mpftp `exec` that outlasts mpftp's wait resets the board over USB
(`rst:0x17 CHIP_USB_UART_RESET`). Under a long-running script that looks like
the firmware crashing. Run such scripts as `main.py`, or with nothing attached.

How the modules are built into the firmware is in
[How esp-vision's modules are built](../how-it-works/esp-vision.md).
