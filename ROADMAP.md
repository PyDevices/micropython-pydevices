# Roadmap

Work here mostly follows bug reports and requests, so if something is broken
or missing for you, open an
[issue](https://github.com/PyDevices/micropython-pydevices/issues). Planned:

- **More modules in CircuitPython-compatible builds.** Next are audioif (it
  needs audiodsp's audio pump built for CircuitPython), usbhost's host engine
  on boards, and castif, h264enc, tflite and vision (ESP-IDF registry
  components on the espressif port). [What builds today](docs/build-plan.md#which-modules-build-on-circuitpython).

## Module roadmaps

The modules plan their own work; each roadmap is next to its code.

- [`audioif`](modules/audioif/ROADMAP.md), the audio hardware layer
- [`cameraif`](modules/cameraif/ROADMAP.md), the MIPI-CSI camera
- [`displayif`](modules/displayif/ROADMAP.md), the display buses and drivers
- [`usbhost`](modules/usbhost/ROADMAP.md), the USB host
