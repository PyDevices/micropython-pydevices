# The USB host, in detail

What the host does today, where its limits are, and why it is built the way
it is. The [README](../README.md) says what usbhost is and how to build it.

Which device classes the packages drive, and which boards run them, are in
[USB host support](https://github.com/PyDevices/pydevices/blob/main/docs/reference/usb-host-support.md).

## Limits, and what has been proven

The controller's
FIFO split is fixed when the host starts, so `usb.core.start(classes=...)` chooses it
from what was asked for (`uvc` leans IN; `uac` without `uvc` makes room for a
speaker that declares packets of up to 384 bytes on the S2/S3, as 48 kHz
stereo dongles do, and a microphone's; otherwise the board's Kconfig
bias), and a camera and a USB Audio 2.0 speaker cannot share one `start()`.
An endpoint bigger than the split takes is refused with `OSError` E2BIG,
and the host prints the split's limits;
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
([`examples/usb_host_msc_simpletest.py`](../examples/usb_host_msc_simpletest.py) is the shortest, and [`examples/usb_drive_mount.py`](../examples/usb_drive_mount.py) lists a hosted stick; [`usb_drive_log.py`](../examples/usb_drive_log.py)
appends sensor lines to one and reads them back after a remount;
[`usb_drive_read.py`](../examples/usb_drive_read.py) reads a file end to end and checks its SHA-256), with
Bulk-Only Reset Recovery and REQUEST SENSE behind it, both exercised by
a deliberate device-side failure rather than assumed; HID is decoded by `usb-host-hid` into
key, button and movement events (the raw reports stay available through `usb.core`); and `host_stop()` on a device that was genuinely held open is now
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
100 Hz.
A device that stops answering a control request no longer leaves the host
half-installed: `host_stop()` powers the root port off to retire the
request the class driver gave up on, and the next `host_start()` works.
A soft reset (Ctrl-D) stops what the program left running, as MicroPython's
own `machine` peripherals do: the host and any stream open on it. The next program finds the host
stopped and its devices attach again. `host_stop()` with a microphone or
speaker stream still open no longer wedges the host.
[`tests/hardware/soft_reset_check.py`](../tests/hardware/soft_reset_check.py) checks all of it on a board.
`host_start()`'s class filter **is** now honoured -- verified both
in the intersection arithmetic and against a live device (excluded from a
class tuple, it is invisible to `host_devices()`; included, it attaches).

## How much isochronous data the host takes

An isochronous IN stream on the ESP32-P4's high-speed port carries one packet in every microframe, which is up to 7.5 MB/s with a 944-byte packet. A Logitech C920e streamed 640x480, 1280x720 and 1920x1080 MJPEG at 30 frames a second on it, 3 to 4 MB/s, and uncompressed 640x480 at 7.4 MB/s with no errors and nothing dropped.

A camera's larger alternate settings promise two or three transactions a microframe, and the host doesn't take those: ESP-IDF's controller driver doesn't set the controller's multi-transaction count, so only the first packet of each microframe arrives and every frame is torn. `usb.host.video` leaves those settings out. A camera that asks for more than one transaction for an MJPEG mode is streamed on the largest single-transaction setting, which works because a real MJPEG frame takes far less than the camera's worst-case reservation.

A slow stream from a webcam is usually the camera. Most lengthen the exposure in a dim room and send fewer frames than the mode says (a C920e asked for 30 a second gave 22 at 640x480, with every microframe polled and no errors). `Camera.hold_frame_rate()` turns that off.

## Keyboards and MIDI controllers behind a hub on the ESP32-P4

The P4's host port is high speed. Plug a keyboard, a mouse or a MIDI
controller into it directly and it works. Put it behind a hub and it won't
enumerate, because a high-speed hub talks to a slower device through a
transaction translator, which the ESP-IDF host doesn't support. The console
shows `Connected device is FS, transaction translator (TT) is not supported`.

Start the host at full speed instead, and the hub runs at full speed too and
passes those devices straight through:

```python
import usb.core

usb.core.start(full_speed=True)
```

High-speed devices on the same hub still work, at full-speed bandwidth
(12 Mbit/s): a webcam negotiates a smaller or more compressed mode, and a
flash drive copies more slowly. So leave it off when everything on the bus is
high speed, or when the slow device is plugged straight into the port. It
takes effect when the host starts; to change it, `usb.core.stop()` and start again. On
the ESP32-S2 and S3 the port is full speed already, and the option changes
nothing. Under the hood it sets the controller's `HCFG.FSLSSupp` bit before
the port is powered.

### Pick the speed once per power-up on the WIFI6 DEV-KIT

On the Waveshare ESP32-P4-WIFI6-DEV-KIT, don't change `full_speed` between
starts. The onboard hub (a CH334F) runs from the board's 3.3 V rail, its
power pins aren't wired to the chip, and the sockets' 5 V is always on, so
nothing the P4 does can take power away from it. `host_stop()` and
`host_port_cycle()` only clear the controller's port-power bit, which stops
the P4 driving the bus; they don't reset the hub, and neither does
`machine.reset()`. A hub that enumerated at high speed and then finds the
host back at full speed (or the other way round, which is untested) can end
up answering nothing at either speed, and only removing the board's power
brings it back
([#121](https://github.com/PyDevices/micropython-pydevices/issues/121)).

So `usb.core.start()` refuses that change on this board. It raises
`USBError` before it touches the host, so the host that is running keeps
running, and the message says what to do: start with the speed you used, or
unplug the board's USB power and start again. Stopping and starting at the
same speed works, and so does changing `classes`. The check remembers the
speed for as long as the interpreter runs, so a soft reset forgets it while
the hub doesn't: choose `full_speed=True` if a keyboard or MIDI controller
will sit behind the hub, and use the same value in every script you run
between power-ups. If H3 sends the host straight to the lower socket the hub
isn't in the path, and `usb.core.start(full_speed=..., force=True)` skips the
check.

## Host and device at once on the ESP32-P4

An ESP32-P4 can be a USB host on one connector and a USB device on another,
with traffic on both at once. The P4 has two USB controllers. The host runs
on the high-speed one, and a board built with `MICROPY_HW_USB_HS (0)` puts
TinyUSB's device on the full-speed one, which the host never touches. Starting
and stopping the host leaves the device attached, so the REPL, a MIDI port or
a sound card stays up on the computer.

On the Waveshare ESP32-P4-WIFI6-DEV-KIT, the `WIFI6_DEV_KIT_H2` variant puts
the device on the USB-C marked "USB1.1 Type-C", which is wired to GPIO24 and
GPIO25, and the host on the USB-A sockets. Out of reset the full-speed
controller gets the PHY on GPIO26 and GPIO27, and USB Serial/JTAG gets GPIO24
and GPIO25. The variant swaps them at boot by setting `sw_hw_usb_phy_sel` and
`sw_usb_phy_sel` in `LP_SYS`'s USB control register (the `USB_PHY_SEL` eFuse
is unburnt, so it doesn't swap them first). The `WIFI6_DEV_KIT` variant keeps
the default, with the device on GPIO26 and GPIO27. On another P4 board, wire
the device's USB-C to the pair your build gives the full-speed controller.

On the DEV-KIT's `WIFI6_DEV_KIT_H2` build, the device ran as a MIDI port
beside the REPL's serial port, with two MIDI devices behind the host's hub: a
keyboard, and a second board sending 20 notes a second. For ten minutes the
board echoed each of 24,900 messages from the computer back to it and wrote
each one to both hosted devices, and passed 12,060 notes from the second board
on to the computer. None went missing, arrived twice or came out of order, and
no transfer failed. Echoes took 2.4 ms at the median and 16 ms at the 99th
percentile, at worst 40 ms.

A board that polls its touch panel over I2C while its device sends resets
within a minute ([#408](https://github.com/PyDevices/micropython-pydevices/issues/408)).
The ten minutes above ran with the touch reads turned off.

## Why the events are drained rather than delivered

The design decision most likely to surprise a reader is that USB events are
buffered in C and drained by Python, instead of arriving as callbacks. That is a measurement, not a preference.

On ESP32 a C callback reaches Python through `mp_sched_schedule`. Measured on
an ESP32-S3 at a 1 kHz event rate:

| Load during the 3 s window | Events delivered | Lost | Worst stall |
|---|---|---|---|
| Python bytecode | 2999 / 3000 | 0% | 1.0 ms |
| `sha256` over 120 KB | 720 / 3002 | 76% | 11.9 ms |
| Flash file writes | 7 / 4070 | **99%** | **1537 ms** |

While the VM sits inside one long C call, no scheduled callback runs at all —
and MSC block writes, display flushes and audio buffering are exactly the
workloads `usbhost` exists to serve. A ring buffer written from interrupt context
cannot be starved that way: a late poll costs latency, which the application
controls, rather than data. Overflow is reported rather than passing in
silence, because the mechanism it replaces failed silently.
