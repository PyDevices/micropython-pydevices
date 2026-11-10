# Roadmap

What's planned for the USB host under `usb.core`. Plans for the class
packages themselves are in pydevices' roadmap.

- TinyUSB's host on more boards, so rp2 and others get `usb.core` too. The
  Arduino GIGA already runs on it; what's left is the other ports, and
  isochronous streams on TinyUSB.
- The FIFO split chosen per stream rather than per `start()`, so a camera and
  a USB Audio 2.0 speaker can share one session.
- An answer to whether an ESP32 can be USB host and USB device at once on two
  connectors: a pedal hosting a MIDI controller while a PC sees its REPL,
  sound card or drive. The GIGA already does it.

Bugs, and things you need that don't work yet, go to
[issues](https://github.com/PyDevices/micropython-pydevices/issues).
