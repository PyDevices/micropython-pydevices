# Roadmap

What's planned for the USB host under `usb.core`. Plans for the class
packages themselves are in pydevices' roadmap.

- TinyUSB's host on more boards, so rp2 and others get `usb.core` too. The
  Arduino GIGA already runs on it; what's left is the other ports, and
  isochronous streams on TinyUSB.
- The FIFO split chosen per stream rather than per `start()`, so a camera and
  a USB Audio 2.0 speaker can share one session.
- Isochronous IN of two and three transactions a microframe on the ESP32-P4, so a camera's
  uncompressed 640x480 at 30 frames a second (18 MB/s) fits; today the host takes one.
- Host and device at once on more ESP32-P4 boards. The WIFI6 DEV-KIT does it
  ([how](docs/host.md#host-and-device-at-once-on-the-esp32-p4)); the
  ESP32-P4-Pico needs a build that gives its header pins the device.

Bugs, and things you need that don't work yet, go to
[issues](https://github.com/PyDevices/micropython-pydevices/issues).
