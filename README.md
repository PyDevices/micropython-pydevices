# usbif

Native USB for MicroPython, both ways round. As a **device**, a board is what
a computer plugs into: a sound card, a webcam, a drive, a keyboard, a MIDI
instrument, through class packages on MicroPython's own `machine.USBDevice`.
As a **host**, it is what a keyboard, thumb drive, MIDI controller, speaker
or camera plugs into, through `usb.core` in PyUSB's and CircuitPython's
shape.

New here? Read the [newcomer's guide](docs/newcomers.md).

## Status: early development

**Nothing is released.** The packages below work on ESP32-S3 and ESP32-P4
boards, at full speed and (on the P4) high speed.

### The board as a USB device

MicroPython's `machine.USBDevice` with micropython-lib's `usb-device`
packages, plus ours in [`packages/`](packages): `usb-device-audio` (a UAC2
speaker), `usb-device-video` (a UVC webcam, YUY2 or MJPEG, at the sizes you
offer), `usb-device-msc` (a drive), and `usb_hid` (CircuitPython's `usb_hid`,
so `adafruit_hid` drives it). MIDI is `midiports.usb_device`, in the
`pydevices` repository. Audio and video stream through endpoints that move
their packets in C, from the USB interrupt: a firmware needs the PyDevices
MicroPython overlay for that (`USBDevice.Ring`, `USBDevice.stream()`).

```python
import usb.device
from usb.device.audio import SpeakerInterface

spk = SpeakerInterface()                       # 48 kHz stereo 16-bit
usb.device.get().init(spk, builtin_driver=True)  # the REPL stays on the cable
```

[`examples/usb_soundcard.py`](examples/usb_soundcard.py) plays what the
computer sends on the board's codec through audiodsp's pump, following the
computer's volume; [`examples/usb_webcam.py`](examples/usb_webcam.py) streams
an ESP32-P4's camera.

### The board as a USB host

[`packages/usb-core`](packages/usb-core) gives `usb.core`, `usb.util` and
`usb_host`; the class packages build on it: `usb-host-msc` (mount a drive),
`usb-host-hid` (keyboards and mice), `usb-host-cdc` (a USB serial device),
`usb-host-audio` (a speaker or microphone), `usb-host-video` (a webcam) and
`usb-host-net` (the internet through a phone's USB tethering). MIDI is `midiports.usb_host`. Adafruit's `adafruit_usb_host_midi` runs on it
unchanged. A loop that polls an endpoint every frame can read with
`dev.read(ep, buf, timeout=1, raise_on_timeout=False)`, as on CircuitPython:
"nothing yet" returns 0 and allocates nothing. `usb.core.running()` and
`usb.core.stats()` say whether the host is up and what it has seen.

```python
import usb.core
from usb.host.msc import Drive
import vfs

drive = Drive(usb.core.find(custom_match=Drive.match))
vfs.mount(vfs.VfsFat(drive), "/usb")
```

A board with no radio can get online through a phone. Turn on USB tethering
on an Android phone, plug it into the board's high-speed host port, and:

```python
from usb.host.net import Tether

net = Tether()
net.active(True)        # the phone gives the board an address by DHCP
```

Sockets, `requests` and `mip` then work over it. On an ESP32-P4 it moves
about 7 Mbit/s, plugged in directly or through a high-speed hub; an
ESP32-S3, whose host port is full speed, about 4 Mbit/s. It speaks
RNDIS, which Samsung and many other Android phones use. CDC-ECM, CDC-NCM and
iPhone tethering are not implemented.

The older host API, `usbif.auto` and its class drivers in C, is still here
while the class packages replace it.

Also working, and the foundation the rest builds on:

- the portable API and its Linux and Windows desktop backends, in `lib/usbif`,
  with one conformance suite run against every backend
- the event transport in C (`src/shared/usbif_ringbuf.c`), with host-side tests
- one small patch to MicroPython (`patches/`), with provenance: an esp32
  helper that lets the host borrow the OTG controller

**Working now, and new:** external hubs, and more than one device at once --
two devices behind a powered hub enumerated together, the module's first
multi-device host session, with the hub itself correctly filtered out for
matching no requested class. (The Phase 2 observation that a drive behind a
hub was invisible turned out to be an IDF Kconfig default, not missing
support.) And **MIDI as a host**: `src/usbif_host_midi.c`, ours because
nobody upstream ships an IDF MIDI host driver -- a real keyboard's
MIDIStreaming interface and endpoints discovered, notes packed and sent,
with the USB-MIDI packet codec split into `shared/` and tested on the host.
Incoming notes are proven too: 989 channel messages read from a keyboard --
notes with velocity, pitch bend across its full range, several CCs, and
channel-10 drums -- with zero bytes dropped.

**Video and host audio, both directions, proven:** a USB webcam hosted on an
ESP32-S3 with its picture on the board's own panel (`usb-host-video`,
`examples/uvc_display.py`), and the board seen as a webcam by Windows
(`usb-device-video`, `examples/usb_webcam.py`). Host audio plays too: the isochronous stall that blocked it was a
`pdMS_TO_TICKS()` rounding to zero ticks at `CONFIG_FREERTOS_HZ=100`, so a
delay that read as 5 ms never yielded at all. If your app feeds a hosted
speaker from the interpreter thread while it also draws or talks to the
network, open it with `Speaker(dev, ring_ms=2000)` and write what
`space()` says fits: a 2 s ring (in PSRAM) rode out 1.2 s stalls on an S3
with no gap, where the default 8 KB ring gapped 5,300 times in 20 s
([#36](https://github.com/PyDevices/usbif/issues/36)). If the audio is made
in C by another native module, the interpreter need not carry it at all:
`Speaker.c_sink()` hands that module a sink it writes from its own
task, described in [src/pcm_c_sink.h](src/pcm_c_sink.h)
([#43](https://github.com/PyDevices/usbif/issues/43)). A microphone can skip
the interpreter too: `Microphone.start(ring)` with an audiodsp
`audiopump.Ring` feeds the pump straight from the bus. A hosted device can
play and record at once: open a `Speaker` and a `Microphone` side by side,
on one device or two. On an ESP32-P4 hosting a USB-C headset dongle with its output
looped into its mic, both ran 30 s with not one packet lost either way
([tests/hardware/uac_duplex_loopback.py](tests/hardware/uac_duplex_loopback.py)).

**Not yet working, said precisely:** macOS
desktop support, which sits at the [community-verified
tier](https://github.com/PyDevices/.github/blob/main/docs/platform-support-tiers.md):
no Mac is on this project's bench, `auto.py` returns a null backend there
rather than pretending, and a report from the field is what promotes it.

**A drive from any block device.** `usb-device-msc` serves a RAM disk, an SD
card or a flash partition: anything with `readblocks`, `writeblocks` and
`ioctl`. When the computer ejects it, the board can mount it again.

And the working host side carries honest limits for now: the controller's
FIFO split is fixed when the host starts, so `host(classes=...)` chooses it
from what was asked for (`uvc` leans IN; `uac` without `uvc` makes room for a
speaker's 196-byte packet and a microphone's; otherwise the board's Kconfig
bias), and a camera and a USB Audio 2.0 speaker cannot share one `start()`;
concurrent sessions are bounded by a
*channel budget*, not by class -- the ESP32-S3 has eight host channels
(`OTG_NUM_HOST_CHAN`), a bulk pair costs two and an interrupt IN costs
one, and every device including a hub costs one for its control pipe.
On the bench that means HID pairs with either MSC or MIDI, while MSC and
MIDI together do not fit. Note the class filter does *not* buy room: it
declines interfaces, not devices, and every enumerated device keeps its
control pipe either way -- so a hub costs two channels before carrying
anything. Host mode is proven at full speed on the ESP32-S3, and at high
speed on the ESP32-P4 with a webcam on a port that supplies 5 V; behind a hub
the P4 reaches full-speed devices when you start the host with
`full_speed=True` (see [below](#keyboards-and-midi-controllers-behind-a-hub-on-the-esp32-p4)). MSC now mounts as a filesystem, read-write
(`examples/usb_drive_mount.py` lists a hosted stick; `usb_drive_log.py`
appends sensor lines to one and reads them back after a remount;
`usb_drive_read.py` reads a file end to end and checks its SHA-256), with
Bulk-Only Reset Recovery and REQUEST SENSE behind it, both exercised by
a deliberate device-side failure rather than assumed; HID delivers raw reports rather than decoded
events; and `host_stop()` on a device that was genuinely held open is now
reliable at realistic hold times (8 of 8 clean in this session's sample)
after two rounds of fixes -- the teardown now closes each class driver's
session properly instead of skipping straight to the raw device handle,
and does so with the event-pump the library's async endpoint-halt/flush
completions need, which nothing was providing once the host task's own
main loop had already exited for shutdown. A narrower race remains at
very short hold times (call `host_stop()` within a fraction of a second
of opening a session and it can still wedge), root-caused to a single
esp-idf call (`usb_host_endpoint_halt()`) blocking indefinitely -- a layer
below anything in this module's own source, not yet closed. A wedged host
now raises `OSError` instead of silently pretending to keep working
either way. Teardown's waits are bounded in milliseconds rather than ticks,
so a board built with a 1000 Hz FreeRTOS tick stops as cleanly as one at
100 Hz ([#29](https://github.com/PyDevices/usbif/issues/29)).
A device that stops answering a control request no longer leaves the host
half-installed: `host_stop()` powers the root port off to retire the
request the class driver gave up on, and the next `host_start()` works
([#73](https://github.com/PyDevices/usbif/issues/73)).
A soft reset (Ctrl-D) stops what the program left running, as MicroPython's
own `machine` peripherals do: the host and any stream open on it, the sound
card's I2S pump, and an attached drive. The next program finds the host
stopped and its devices attach again. `host_stop()` with a microphone or
speaker stream still open no longer wedges the host.
`tests/hardware/soft_reset_check.py` checks all of it on a board.
`host_start()`'s class filter **is** now honoured -- verified both
in the intersection arithmetic and against a live device (excluded from a
class tuple, it is invisible to `host_devices()`; included, it attaches) --
and `capabilities()` reports the true built set (`{'cdc', 'hid', 'msc'}` on
the S3 bench build) rather than the empty set it silently returned since
Phase 1.
The API design is in [`docs/api-sketch.md`](docs/api-sketch.md).

What's planned next is in [ROADMAP.md](ROADMAP.md).

### Keyboards and MIDI controllers behind a hub on the ESP32-P4

The P4's host port is high speed. Plug a keyboard, a mouse or a MIDI
controller into it directly and it works. Put it behind a hub and it won't
enumerate, because a high-speed hub talks to a slower device through a
transaction translator, which the ESP-IDF host doesn't support. The console
shows `Connected device is FS, transaction translator (TT) is not supported`.

Start the host at full speed instead, and the hub runs at full speed too and
passes those devices straight through:

```python
import usbif.auto

host = usbif.auto.host(classes=("midi", "hid"), full_speed=True).start()
```

High-speed devices on the same hub still work, at full-speed bandwidth
(12 Mbit/s): a webcam negotiates a smaller or more compressed mode, and a
flash drive copies more slowly. So leave it off when everything on the bus is
high speed, or when the slow device is plugged straight into the port. It
takes effect when the host starts; to change it, `stop()` and start again. On
the ESP32-S2 and S3 the port is full speed already, and the option changes
nothing. Under the hood it sets the controller's `HCFG.FSLSSupp` bit before
the port is powered.

## Why the events are drained rather than delivered

The design decision most likely to surprise a reader is that USB events are
buffered in C and collected by Python calling `poll()`, instead of arriving as
callbacks. That is a measurement, not a preference.

On ESP32 a C callback reaches Python through `mp_sched_schedule`. Measured on
an ESP32-S3 at a 1 kHz event rate:

| Load during the 3 s window | Events delivered | Lost | Worst stall |
|---|---|---|---|
| Python bytecode | 2999 / 3000 | 0% | 1.0 ms |
| `sha256` over 120 KB | 720 / 3002 | 76% | 11.9 ms |
| Flash file writes | 7 / 4070 | **99%** | **1537 ms** |

While the VM sits inside one long C call, no scheduled callback runs at all —
and MSC block writes, display flushes and audio buffering are exactly the
workloads `usbif` exists to serve. A ring buffer written from interrupt context
cannot be starved that way: a late poll costs latency, which the application
controls, rather than data. Overflow is reported rather than passing in
silence, because the mechanism it replaces failed silently.

## Building

usbif does its work on the ESP32-S2, S3 and P4, the parts with a USB OTG
controller; the CMake glue skips every other esp32 part and rp2. One step
comes first, and skipping it builds a module whose USB functions are silently
absent rather than one that fails loudly: **apply the patches** to your
MicroPython tree. The device functions reach the host through hooks this
module adds to MicroPython's shared TinyUSB glue, and the host side needs the
esp32 OTG helper. All four apply to MicroPython v1.29.0:

```bash
/path/to/usbif/apply_patches.sh --apply /path/to/micropython   # --status to check, --revert to undo
```

Your board needs nothing of its own: the CMake glue defines the TinyUSB
extension header and MSC for the build itself. The ESP-IDF settings the host
side wants (hub support, a larger control-transfer buffer, and on the S3 a
FIFO split that favours IN) are in [sdkconfig](sdkconfig) and
[sdkconfig.esp32s3](sdkconfig.esp32s3). micropython-pydevices' `build_mp.py`
appends them whenever usbif is selected; add them to your board's
`SDKCONFIG_DEFAULTS` otherwise.

The sound card wants TinyUSB 0.19 or later, which re-arms its endpoint in
the USB interrupt. On the older TinyUSB that MicroPython 1.29's esp32 port
builds, the endpoint waits for the interpreter, and a busy application loses
5 to 13 % of the stream, heard as a tick
([#39](https://github.com/PyDevices/usbif/issues/39)).
micropython-pydevices' patch 0014 moves the ESP32-P4 to 0.21. The S2 and S3
stay on 0.18 for now ([#48](https://github.com/PyDevices/usbif/issues/48)), so
an S3 sound card still ticks under load. usbif builds either way.

Then, on MicroPython 1.29 or later, add one line to the manifest your board
build already uses (usually `ports/esp32/boards/manifest.py`, unless your board
brings its own):

```python
include("/path/to/usbif/manifest.py")
```

and build as usual. That line brings both halves: it names the C module
(`_usbif`) and freezes the `usbif` Python package from `lib/`. The package
imports `events`, which comes from
[pydevices](https://github.com/PyDevices/pydevices) by `mip`, so install that
on the board too. If you would rather pass a manifest of your own as
`FROZEN_MANIFEST=`, it replaces the port's default, so give it
`include("$(PORT_DIR)/boards/manifest.py")` as well or you lose `asyncio` and
the port's other frozen modules.

On MicroPython older than 1.29, manifests have no `c_module()`, so this one
will not load. Use the `USER_C_MODULES` flag, and freeze `lib/usbif` yourself
(`package("usbif", base_path="/path/to/usbif/lib")` in your manifest) or copy
it to the board:

```bash
idf.py -D MICROPY_BOARD=ESP32_GENERIC_S3 -D USER_C_MODULES=/path/to/usbif/micropython.cmake build
```

Without both steps the build succeeds and the board simply enumerates without
audio or MIDI -- quiet rather than obviously broken, which is why it is
called out here. One caution learned the hard way
and filed upstream as
[micropython#19667](https://github.com/micropython/micropython/issues/19667):
do not pass `BUILD=` to the esp32 port's `make`. It propagates into the
mpy-cross sub-make, which then plants its own qstr fragments in your build
directory and breaks the link. Use `idf.py -B` for an out-of-tree build
instead, which does not inherit the variable.

## Examples

| File | Role | What it shows |
|---|---|---|
| [`usb_soundcard.py`](examples/usb_soundcard.py) | device | The board as a USB sound card, played through audiodsp's pump |
| [`usb_webcam.py`](examples/usb_webcam.py) | device | The board's camera as a USB webcam, MJPEG at the size the computer picks |
| [`hid_keyboard.py`](examples/hid_keyboard.py) | device | Board types into the host |
| [`hid_mouse.py`](examples/hid_mouse.py) | device | Board moves the host cursor |
| [`ram_drive.py`](examples/ram_drive.py) | device | A RAM disk as a USB drive (`usb-device-msc`) |
| [`sd_drive.py`](examples/sd_drive.py) | device | An SD card as a USB drive (`usb-device-msc`) |
| [`midi_harmonizer.py`](examples/midi_harmonizer.py) | device | MIDI effect: melody in, triads out |
| [`midi_harmonizer_ui.py`](examples/midi_harmonizer_ui.py) | device | Harmonizer with a touchscreen chord picker |
| [`midi_device_in.py`](examples/midi_device_in.py) | device | Prove the board receives MIDI from a host |
| [`host_enum.py`](examples/host_enum.py) | host | Attach/detach via the portable API (board or desktop) |
| [`hid_host.py`](examples/hid_host.py) | host | USB keyboard → PyDevices key events (M1) |
| [`usb_serial.py`](examples/usb_serial.py) | host | CDC read/write to a USB-serial device |
| [`usb_speaker.py`](examples/usb_speaker.py) | host | Play through a hosted USB speaker, or a board running `usb_soundcard.py` |
| [`usb_mic.py`](examples/usb_mic.py) | host | Capture from a hosted USB microphone |
| [`usb_drive_mount.py`](examples/usb_drive_mount.py) | host | Mount a flash drive and list files |
| [`usb_drive_log.py`](examples/usb_drive_log.py) | host | Append sensor lines to a hosted stick |
| [`midi_host.py`](examples/midi_host.py) | host | Host a MIDI keyboard; send a chord back |
| [`uvc_display.py`](examples/uvc_display.py) | host | Hosted webcam on the board's panel (MJPEG via `jpegio` when present) |

Board-to-board pairings, VBUS warnings, and firmware holes (by issue number)
live in [`examples/README.md`](examples/README.md).

## Tests

The ring buffer is tested on the host, where a failure is a two-second answer
rather than a reflash:

```bash
cc -std=c11 -Wall -Wextra -Werror -Isrc -o /tmp/test_ringbuf tests/test_ringbuf.c src/shared/usbif_ringbuf.c && /tmp/test_ringbuf
cc -std=c11 -Wall -Wextra -Werror -Isrc -o /tmp/test_byte_ring tests/test_byte_ring.c src/shared/usbif_byte_ring.c && /tmp/test_byte_ring
```

The portable API's conformance suite lives with the API, in this repository
(it finds `events` and `audiodev` in a sibling `pydevices` checkout):

```bash
python -m unittest discover -s tests -p "test_usbif.py"
```

## Licence

MIT.
