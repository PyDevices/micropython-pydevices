# usbhost examples

One script per use of the shipping API. Each role is class-compliant on its
own: a PC or a commercial peripheral is a valid other end. Board-to-board
pairing is the same scripts on two boards, not a special `pair_*` file.

## Class × role

| Class | Device (board presents as…) | Host (board drives…) |
|---|---|---|
| **UAC** | [`usb_soundcard.py`](usb_soundcard.py) (through audiodsp's pump) | [`usb_speaker.py`](usb_speaker.py), [`usb_mic.py`](usb_mic.py) |
| **UVC** | [`usb_webcam.py`](usb_webcam.py) (MJPEG or YUY2, at the size the host picks) | [`uvc_display.py`](uvc_display.py) (MJPEG via `jpegio` when present) |
| **MIDI** | [`midi_harmonizer.py`](midi_harmonizer.py), [`midi_harmonizer_ui.py`](midi_harmonizer_ui.py), [`midi_device_in.py`](midi_device_in.py) | [`midi_host.py`](midi_host.py) |
| **HID** | [`hid_keyboard.py`](hid_keyboard.py), [`hid_mouse.py`](hid_mouse.py) | [`hid_host.py`](hid_host.py) |
| **MSC** | [`sd_drive.py`](sd_drive.py), [`ram_drive.py`](ram_drive.py) | [`usb_drive_mount.py`](usb_drive_mount.py), [`usb_drive_read.py`](usb_drive_read.py), [`usb_drive_log.py`](usb_drive_log.py) |
| **CDC** | MicroPython's own console | [`usb_serial.py`](usb_serial.py) |
| **enum** | — | [`host_enum.py`](host_enum.py) (lists what is plugged in, watches attach and detach) |

## Board-to-board pairings

Roles follow the hardware each board has, not their size. The P4 is the
device in these pairings. It can host too, but only on a port that supplies
5 V, such as the ESP32-P4-WIFI6-DEV-KIT's direct Type-A socket, where
`uvc_display.py` streams a webcam. The Touch-LCD-4B's USB-C port only takes
power in, so it can't host without 5 V from outside.

| Device board | Script | Host board | Script | What is lent |
|---|---|---|---|---|
| **P4** | `usb_soundcard.py` | **S3** | `usb_speaker.py` | Sound output -- the headline offload |
| **P4** | `usb_webcam.py` | **S3** | `uvc_display.py` | Camera → panel |
| **P4** | `sd_drive.py` | **S3** | `usb_drive_mount.py`, `usb_drive_read.py` | Shared storage |
| **P4** / **S3** | `midi_harmonizer.py` | **S3** | `midi_host.py` | MIDI effect / instrument |
| **S3** | `hid_keyboard.py` | **S3** | `hid_host.py` | Control surface (needs two S3s, or S3-device + PC) |

A PC can replace either end. That is the point of standard classes.

### Headline: P4 as a USB sound card for an S3

1. Console on each board's **UART bridge**, not on the OTG port under test.
2. P4: `mpftp run examples/usb_soundcard.py` -- enumerates as Speakers, played
   by audiodsp's pump into the ES8311.
3. S3: OTG adapter or powered hub on the host port (the Waveshare S3 touch
   boards do not switch VBUS; see also [usbif#5](https://github.com/PyDevices/usbif/issues/5)).
4. S3: `mpftp run examples/usb_speaker.py` -- finds the P4, plays a 440 Hz tone.
5. Hear it on the P4's speaker.

### Power

Connecting two self-powered boards can **back-feed** VBUS and leave a board
silent and unflashable. Default link: a cable that omits VBUS, or a powered
hub between them. Unplug both USB cables and use UART alone to recover a
wedged board.

## Firmware holes (do not invent an example)

| Hole | Issue | Consequence for examples |
|---|---|---|
| Device UAC is speaker-only (no mic endpoint) | [Planned](../../pydevices/ROADMAP.md) | `usb_mic.py` hosts a commercial mic; no device-side capture script |
| P4 behind a hub: ESP-IDF's host has no transaction translator, so a full-speed device is refused at high speed | [#120](https://github.com/PyDevices/micropython-pydevices/issues/120) | A keyboard or MIDI controller behind a hub needs `usb.core.start(full_speed=True)` (`FULL_SPEED = True` in `hid_host.py`, `midi_host.py` and `host_enum.py`); plugged straight into the port it needs nothing. At full speed the whole port is full speed, so a webcam on the same hub drops to full-speed bandwidth and the two cannot be mixed. Restarting the host at full speed after a high-speed run can leave the DEV-KIT's hub dead until a power cycle ([#121](https://github.com/PyDevices/micropython-pydevices/issues/121)) |
| Host FIFO split is per session | [Planned](../ROADMAP.md) | `usb.core.start(classes=...)` decides it: `uvc` leans IN (600 B), `uac` without `uvc` gives periodic OUT 200 B and IN 528 B on an S3, four times that on a P4 (800 B and 2136 B); a camera and a 2.0 speaker cannot share one `start()` |

## Conventions

- Headers name the use, the board role, the other end, and whether UART must
  hold the REPL (a device setup without the console cuts a native-USB session).
- Device examples use `machine.USBDevice` and the class packages; host
  examples use `usb.core` and its class packages.
  Never `_usbhost` directly: the C module is the packages' own. `tests/test_no_usbhost_leak.py` enforces it.
- No measured fps or latency numbers are claimed in headers for scripts that
  have not been re-run in this pass -- cite prior evidence or stay quiet.
