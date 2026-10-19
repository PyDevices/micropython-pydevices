# Roadmap

Work here mostly follows bug reports and requests, so if something is broken
or missing for you, open an
[issue](https://github.com/PyDevices/micropython-pydevices/issues). Planned:

- **More modules in CircuitPython-compatible builds.** The ESP32 boards build
  now, against ESP-IDF 6, with definitions for the Waveshare ESP32-S3 and
  ESP32-P4 boards. Next is displayif's desktop display, then the modules that
  don't build there yet.
- **Freezing our Python packages into CircuitPython-compatible builds**,
  through CircuitPython's own frozen-module mechanism.

## Module roadmaps

The modules plan their own work; each roadmap is next to its code.

- [`audioif`](modules/audioif/ROADMAP.md), the audio hardware layer
- [`cameraif`](modules/cameraif/ROADMAP.md), the MIPI-CSI camera
- [`displayif`](modules/displayif/ROADMAP.md), the display buses and drivers
- [`usbhost`](modules/usbhost/ROADMAP.md), the USB host
