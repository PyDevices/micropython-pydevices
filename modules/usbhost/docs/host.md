# The USB host, in detail

What the host does today, where its limits are, and why it is built the way
it is. The [README](../README.md) says what usbhost is and how to build it.

## Limits, and what has been proven

The controller's
FIFO split is fixed when the host starts, so `usb.core.start(classes=...)` chooses it
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
100 Hz.
A device that stops answering a control request no longer leaves the host
half-installed: `host_stop()` powers the root port off to retire the
request the class driver gave up on, and the next `host_start()` works.
A soft reset (Ctrl-D) stops what the program left running, as MicroPython's
own `machine` peripherals do: the host and any stream open on it. The next program finds the host
stopped and its devices attach again. `host_stop()` with a microphone or
speaker stream still open no longer wedges the host.
`tests/hardware/soft_reset_check.py` checks all of it on a board.
`host_start()`'s class filter **is** now honoured -- verified both
in the intersection arithmetic and against a live device (excluded from a
class tuple, it is invisible to `host_devices()`; included, it attaches).

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
