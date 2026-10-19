# usb-device-midi: a burst fills the schedule queue and the device stops hearing MIDI

Fixed upstream already, so there's nothing to report. micropython-lib fixed it on 2026-09-16,
in `90a3385` ("usb-device-midi: Queue at most one micropython.schedule() for rx") and `4fa59bd`
("usb-device-midi: Resume RX endpoint transfer after handling callback"), which make
`usb-device-midi` 0.1.1. MicroPython's `lib/micropython-lib` still points at `ee4bb8f`
(2026-08-17), which has 0.1.0, so a firmware that freezes `usb-device-midi` from the
submodule, or a board whose `/lib` is copied from it, gets the bug. `mip install
usb-device-midi` gets 0.1.1 from the index and is fine.

We carry both commits unchanged as
[patches/micropython-lib/](../../patches/micropython-lib/). Once MicroPython's submodule
passes `4fa59bd`, `apply_patches.py` prints `already in micropython-lib` for each of
them, and the directory can go.

## What it looks like

With 0.1.0, a computer sends 200 note-ons back to back to an ESP32-P4 (high speed) or
ESP32-S3 (full speed), one 4-byte event per packet, about 0.3 ms apart. After 12 to
15 of them, the board prints:

```
Traceback (most recent call last):
  File "/lib/usb/device/core.py", line 342, in _xfer_cb
  File "/lib/usb/device/midi.py", line 152, in _rx_cb
RuntimeError: schedule queue full
```

and never receives MIDI again, because the device stops re-arming the OUT endpoint. The
computer's next send then blocks.

TinyUSB's task runs as a scheduler node, so the scheduler stays locked while
`tud_task_ext()` handles every completion that lands during the pass. Each completion
re-arms the endpoint, the host's next packet completes before the pass ends, and each
one schedules another `_on_rx`. A probe that counted those calls saw all 8 slots of
`MICROPY_SCHEDULER_DEPTH` taken by `_on_rx` alone. In 0.1.0, `schedule()` comes before
the re-arm and raises, so the re-arm never happens. Catching that error isn't enough
on its own, either: the 64-byte receive buffer then fills with no `_on_rx` queued to
drain it and re-arm.

0.1.1 queues at most one `_on_rx` at a time, re-arms in a `finally`, and has `_on_rx`
re-arm once it has drained the buffer. With it the same boards echo 200 of 200 in
order in a burst (0.51 s on the P4, 1.21 s on the S3) and 600 of 600 on the P4.

## If you'd like MicroPython to move the submodule

A comment for a MicroPython pull request or issue that bumps `lib/micropython-lib`:

> `lib/micropython-lib` is at `ee4bb8f`, which predates micropython-lib `90a3385` and
> `4fa59bd` (usb-device-midi 0.1.1). With 0.1.0 frozen, a USB MIDI device that receives
> a fast burst raises `RuntimeError: schedule queue full` in `_rx_cb` and stops
> re-arming its OUT endpoint, so it never receives again. Moving the submodule past
> `4fa59bd` fixes it for frozen builds.
