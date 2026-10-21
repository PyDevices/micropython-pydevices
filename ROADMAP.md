# Roadmap

Work here mostly follows bug reports and requests, so if something is broken
or missing for you, open an
[issue](https://github.com/PyDevices/micropython-pydevices/issues).

More modules in CircuitPython-compatible builds
([what builds today](docs/build-plan.md#which-modules-build-on-circuitpython)):

- **castif casting on CircuitPython's ESP32-P4 builds.** castif and h264enc
  build there and h264enc encodes, but the port has no network until
  CircuitPython drives the boards' ESP32-C6 radio, so a cast can't start yet.
- **More of usbhost on a CircuitPython ESP32-P4 board.** The host enumerates
  and answers a wireless keyboard-and-mouse dongle on the WIFI6 DEV-KIT. Still
  to try there: a drive, a webcam's stream and a phone. Tethering waits for the
  port's network.
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
