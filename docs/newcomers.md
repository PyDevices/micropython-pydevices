# Newcomer's guide to usbif

`usbif` gives a MicroPython board USB both ways round. As a device, a board appears to a computer as a sound card, webcam, drive, keyboard or MIDI instrument. As a host, it drives keyboards, drives, MIDI controllers, serial devices, speakers, microphones and cameras.

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
report = kbd.read()
```

Start the host with the classes you'll stream (`usb.core.start(classes=("uvc",))` for a camera, `("uac",)` for a speaker): the controller's FIFO split is fixed while the host runs.

The older host API, `usbif.auto` with its class drivers in C, is still in `lib/usbif/` while the packages replace it.

## Repository map

| Path | Purpose |
|---|---|
| `packages/` | The class packages: `usb-device-audio`, `usb-device-video`, `usb-device-msc`, `usb-hid`, `usb-core`, `usb-host-*`. |
| `src/mod_usbif.c` | The C module: the host session and its transfers. |
| `src/usbif_host*.c` | The host core, the generic transfer layer under `usb.core`, and the older class drivers. |
| `src/shared/` | Event ring, PCM sink and USB-MIDI packet code. |
| `lib/usbif/` | The older portable host API and its desktop backends. |
| `patches/` | The ESP32 patch that lets the host borrow the OTG controller. |
| `examples/` | Device and host examples. |
| `tools/` | The device proof matrix and the stream and host checks. |
| `tests/` | Native ring, leak and API tests. |

## Safe first contributions

Start with an example or a focused test. Don't call Python from a USB interrupt: streams stay in C, and everything else reaches Python through the scheduler or a poll.
