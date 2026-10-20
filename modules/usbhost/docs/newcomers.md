# Newcomer's guide to usbhost

`usbhost` is the C module under a MicroPython board's USB packages, which live in [pydevices](https://github.com/PyDevices/pydevices). Together they give a board USB both ways round. As a device, a board appears to a computer as a sound card, webcam, drive, keyboard or MIDI instrument. As a host, it drives keyboards, drives, MIDI controllers, serial devices, speakers, microphones and cameras.

## The board as a device

Device classes are Python packages on MicroPython's own `machine.USBDevice`, with micropython-lib's `usb-device` core:

```python
import usb.device
from usb.device.audio import SpeakerInterface

spk = SpeakerInterface()                         # a 48 kHz stereo speaker
usb.device.get().init(spk, builtin_driver=True)  # beside the board's console
```

`init()` re-enumerates the board, because a USB device can't change what it is in place. Put several interfaces in one `init()` to present them together. A controller has a fixed number of endpoints (the ESP32-S3 drives IN endpoints 1 to 4), and `machine.USBDevice` refuses a set that doesn't fit with a `ValueError` naming the endpoint.

Audio and video move their packets in C, from the USB interrupt, through `USBDevice.Ring` and `USBDevice.stream()`. A firmware needs the PyDevices MicroPython overlay for those.

## The board as a host

Host classes are Python packages on `usb.core`:

```python
import usb.core
from usb.host.hid import Device, KEYBOARD

kbd = Device(usb.core.find(custom_match=Device.match(KEYBOARD)))
event = kbd.read()      # None, or (KEY_DOWN, keycode, modifiers) with adafruit_hid's Keycode values
```

Start the host with the classes you'll stream (`usb.core.start(classes=("uvc",))` for a camera, `("uac",)` for a speaker): the controller's FIFO split is fixed while the host runs.

## Repository map

The class packages (`usb-core`, `usb-host-*`, `usb-device-*`, `usb-hid`) are in pydevices, each in its own directory beside `midiports`. The ESP32 patch that lets the host borrow the OTG controller is the overlay's `patches/micropython/0039`.

| Path | Purpose |
|---|---|
| `src/mod_usbhost.c` | The C module: the host session, its transfers and streams. |
| `src/usbhost_host*.c` | The host core, the generic transfers under `usb.core`, the isochronous streams under the audio and video packages, and the tethering adapter. |
| `src/shared/` | The event ring, the PCM sink's gate, and the host streams' pacing, framing and queue sizing. |
| `examples/` | Device and host examples. |
| `tools/` | The device proof matrix and the stream and host checks. |
| `tests/` | Native ring, stream and sink tests, and the examples' tests. |

## Safe first contributions

Start with an example or a focused test. Don't call Python from a USB interrupt: streams stay in C, and everything else reaches Python through the scheduler or a poll.
