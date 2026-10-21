# Roadmap

Work here mostly follows bug reports and requests, so if something is broken
or missing for you, open an
[issue](https://github.com/PyDevices/micropython-pydevices/issues).

More modules in CircuitPython-compatible builds
([what builds today](docs/build-plan.md#which-modules-build-on-circuitpython)):

- **Python-generated audio on CircuitPython.** A stream sample, the audio
  pump's C side, that plays a ring your Python code fills through
  `audiobusio.I2SOut`. CircuitPython's own samples take only C; audioif waits on it.
- **castif casting on CircuitPython's ESP32-P4 builds.** castif and h264enc
  build there and h264enc encodes, but the port has no network until
  CircuitPython drives the boards' ESP32-C6 radio, so a cast can't start yet.
- **usbhost on CircuitPython for the ESP32-P4.** CircuitPython's ESP32 builds
  have no USB host; the P4's second USB controller can run ours. The S3's one
  controller is CircuitPython's drive and REPL, so the S3 is left out.
- **The USB host drivers on CircuitPython's own `usb.core`**, on the RP2040
  and i.MX: planned in [pydevices](https://github.com/PyDevices/pydevices/blob/main/ROADMAP.md#usb).

tflite and vision stay MicroPython-only: they are esp-vision's modules, not
ours to port.

## Module roadmaps

The modules plan their own work; each roadmap is next to its code.

- [`audioif`](modules/audioif/ROADMAP.md), the audio hardware layer
- [`cameraif`](modules/cameraif/ROADMAP.md), the MIPI-CSI camera
- [`displayif`](modules/displayif/ROADMAP.md), the display buses and drivers
- [`usbhost`](modules/usbhost/ROADMAP.md), the USB host
