# Roadmap

usbif is heading toward every USB role a board can play, in both directions,
on the boards people actually use.

## Device classes

- A USB microphone: a capture endpoint on the device side, so a board can be a
  USB mic or a full-duplex sound card for a PC or another board.
- `network.USBD_NCM` (Ethernet over USB, new in MicroPython 1.29): either a
  package for it, or a note on enabling it without usbif.

## MIDI

- USB MIDI gives byte-stream ports that CircuitPython's `adafruit_midi` reads,
  on the device side by extending MicroPython's `usb-device-midi`. usbif's own
  MIDI port class and parser retire.

## Closer to MicroPython

- usbif rebuilds on MicroPython's own USB support: device classes become
  packages on `machine.USBDevice`, with what MicroPython lacks added in the
  same style. Code written for usbif's current API will need changing.
- Every class works at full and high speed, choosing its packet sizes when the
  host connects, so a P4's high-speed port streams audio and video as well as
  its full-speed one.
- Host mode follows, in CircuitPython's `usb.core` shape, without losing
  anything usbif's host does today. It stays on ESP-IDF's USB host library on
  the ESP32 chips; a TinyUSB host, for other chips, comes after.
- Keyboards, mice and other HID devices take CircuitPython's `adafruit_hid`
  API, so the same keyboard code runs over USB and over BLE.

## Host and device together

- An answer to whether a board can be USB host and USB device at once on two
  connectors: a pedal hosting a MIDI controller while a PC sees its REPL,
  sound card or drive.

Bugs, and things you need that don't work yet, go to
[issues](https://github.com/PyDevices/usbif/issues).
