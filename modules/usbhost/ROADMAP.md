# Roadmap

What's planned for the USB host under `usb.core`. Plans for the class
packages themselves are in pydevices' roadmap.

- A TinyUSB host backend beside ESP-IDF's, so boards without ESP-IDF's host
  library (rp2 and others) get `usb.core` too. The C binding stays
  port-neutral so this can follow.
- The FIFO split chosen per stream rather than per `start()`, so a camera and
  a USB Audio 2.0 speaker can share one session.
- An answer to whether a board can be USB host and USB device at once on two
  connectors: a pedal hosting a MIDI controller while a PC sees its REPL,
  sound card or drive.

Bugs, and things you need that don't work yet, go to
[issues](https://github.com/PyDevices/micropython-pydevices/issues).
