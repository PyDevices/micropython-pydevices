# usbhost

The C half of USB on a PyDevices MicroPython firmware: `_usbhost`, the USB
host port and the transfers and isochronous streams that `usb.core` and the
`usb.host.*` packages are built on. You don't import it yourself. You use the
packages, and they use it.

It builds on the ESP32-S2, S3 and P4, the parts with a USB OTG controller, on
ESP-IDF's USB host library. Its CMake glue skips every other part, so a
firmware for those still builds, without the host.

On CircuitPython the host builds for the ESP32-P4, where the boards leave the
high-speed controller free. On the Waveshare ESP32-P4-WIFI6-DEV-KIT it
enumerates a webcam and a wireless keyboard-and-mouse dongle behind the
board's hub (started with `full_speed=True`) and answers the dongle's HID
requests; drives and streams haven't been tried there. Phone tethering is
left out there, because the port has no network yet. Every other CircuitPython build gets `_usbhost`
without the host.

## The packages are in pydevices

Everything you call lives in [pydevices](https://github.com/PyDevices/pydevices),
beside `midiports`, and installs with `mip`:

- `usb-core`: `usb.core`, `usb.util` and `usb_host`, in PyUSB's and
  CircuitPython's shape.
- `usb-host-msc`, `usb-host-hid`, `usb-host-cdc`, `usb-host-audio`,
  `usb-host-video`, `usb-host-net`: a drive, a keyboard or mouse, a USB serial
  device, a speaker or microphone, a webcam, and the internet through a
  phone's USB tethering.
- `usb-device-audio`, `usb-device-video`, `usb-device-msc` and `usb-hid`: the
  board as a sound card, a webcam, a drive or a keyboard, on MicroPython's own
  `machine.USBDevice`. These need no `_usbhost`; audio and video need the
  overlay's streaming endpoints (`USBDevice.Ring`, `USBDevice.stream()`).

```python
mip.install("github:PyDevices/pydevices/usb-host-msc")   # brings usb-core with it
```

```python
import usb.core
from usb.host.msc import Drive
import vfs

drive = Drive(usb.core.find(custom_match=Drive.match))
vfs.mount(vfs.VfsFat(drive), "/usb")
```

What the host can and can't do yet, keyboards behind a hub on the P4, and
why events are drained rather than called back:
[docs/host.md](docs/host.md).

## Building

Name it in a `build_mp.py` build, or take `all`:

```bash
python3 build_mp.py --port esp32 --board ESP32_GENERIC_S3 --variant SPIRAM_OCT --modules usbhost
```

The build applies what the host needs on its own: overlay patch 0039 lets
the host borrow the OTG controller from the device stack, and the ESP-IDF
settings in [sdkconfig](sdkconfig) and [sdkconfig.esp32s3](sdkconfig.esp32s3)
(hub support, a larger control-transfer buffer, and on the S3 a FIFO split
that favours IN) are appended whenever usbhost is selected.

On CircuitPython, `build_mp.py` also fetches Espressif's `usb` component,
which holds the USB host library from ESP-IDF 6 on:

```bash
python3 build_mp.py --interpreter circuitpython --port espressif --board waveshare_esp32p4_wifi6_touch_lcd_4b --modules usbhost
```

## Examples

Device and host examples, one script per use, are in
[examples/](examples/README.md). They import the packages, never `_usbhost`.

## Tests

The host-testable C runs anywhere with a C compiler, and the Python checks
on CPython; [.github/workflows/usbhost.yml](../../.github/workflows/usbhost.yml)
runs both and the unix build:

```bash
cc -std=c11 -Wall -Wextra -Werror -Isrc -o /tmp/test_ringbuf tests/test_ringbuf.c src/shared/usbhost_ringbuf.c && /tmp/test_ringbuf
cc -std=c11 -Wall -Wextra -Werror -Isrc -o /tmp/test_iso_stream tests/test_iso_stream.c && /tmp/test_iso_stream
python -m unittest discover -s tests -p "test_*.py"
```

The proof matrix and the stream and host checks for real boards are in
[tools/](tools/). The packages' own tests are in pydevices.

## Licence

MIT.
