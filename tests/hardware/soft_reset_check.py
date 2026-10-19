"""Soft reset check: usbif shuts its hardware down on a soft reset.

Run on the PC, with CPython and ``mpftp`` on the PATH (or named by ``MPFTP``),
against a board running usbif with a USB audio device on its host port: the
ESP32-P4 DEV-KIT with a mic in its USB-A hub, say, or a USB speaker or DAC.

    python tests/hardware/soft_reset_check.py -d COM55 --cycles 5

Each cycle runs a short program on the board through the raw REPL, the way an
app would, and leaves everything running for the soft reset to deal with:

- the USB host, started at full speed for the UAC class (``usb.core``);
- an audio stream, open and moving: the device's microphone through
  ``usb.host.audio.Microphone`` when it has one, otherwise its speaker
  through ``Speaker``, fed silence.

Then it soft-resets the board and goes again. From the second cycle on, each
check expects a fresh start:

- ``host stopped``: the host isn't running before the program starts it;
- ``attach event``: starting the host queues an attach for the device, so the
  host really started again rather than finding the old one;
- ``stream opens``: the stream opens and moves audio within a second (bytes
  recorded for a mic, bytes sent on the bus for a speaker).

Prints ``PASS``/``FAIL`` per check and a final ``GATE PASS``/``GATE FAIL``.

The checker can fail, and did. On firmware from before the fix, the second
cycle found the host still running (``host stopped`` FAIL), no attach event,
and the mic's stream still open from the first cycle, so opening it again
failed. Stopping the host from Python at that point didn't help: the stream's
transfers never retired and the host came back wedged until a hard reset.
"""

import argparse
import json
import os
import shlex
import subprocess
import sys

BOARD_STEP = r"""
import time, _usbif
def say(*a):
    print("@@", *a)
say("host_running_before", _usbif.host_stats()[0])
import usb.core
from usb.host.audio import Microphone, Speaker
usb.core.start(full_speed=True, classes=("uac",))
t0 = time.ticks_ms()
attached = False
while time.ticks_diff(time.ticks_ms(), t0) < 10000 and not attached:
    events, _ = _usbif.host_drain(16)
    attached = any(kind == 0 and "uac" in row[5] for kind, row in events)
    time.sleep_ms(50)
say("attach_event", attached)
# The stream is left open on purpose: the soft reset has to stop it.
got = 0
try:
    dev = usb.core.find(custom_match=Microphone.match)
    if dev is not None:
        say("stream", "mic")
        mic = Microphone(dev, ring_ms=500)
        mic.start()
        buf = bytearray(960)
        t1 = time.ticks_ms()
        while time.ticks_diff(time.ticks_ms(), t1) < 1000:
            got += mic.read(buf)
            time.sleep_ms(10)
    else:
        dev = usb.core.find(custom_match=Speaker.match)
        if dev is None:
            raise OSError("no USB audio device")
        say("stream", "speaker")
        spk = Speaker(dev, ring_ms=500)
        spk.start()
        buf = bytearray(960 - 960 % spk.frame_bytes)
        t1 = time.ticks_ms()
        while time.ticks_diff(time.ticks_ms(), t1) < 1000:
            spk.write(buf)
            time.sleep_ms(10)
        got = spk.stats()[1]
except Exception as e:
    say("stream_error", repr(e))
say("stream_bytes", got)
"""

def mpftp(args, device):
    cmd = shlex.split(os.environ.get("MPFTP", "mpftp"))
    result = subprocess.run(cmd + [args[0], "-d", device] + args[1:],
                            capture_output=True, text=True, timeout=120)
    try:
        return json.loads(result.stdout)
    except ValueError:
        return {"ok": False, "error": (result.stdout + result.stderr).strip()}


def run_step(device, code):
    reply = mpftp(["exec", code], device)
    text = reply.get("output") or reply.get("error") or ""
    values = {}
    for line in text.splitlines():
        if line.startswith("@@ "):
            key, _, value = line[3:].partition(" ")
            values[key] = value.strip()
    return values, text


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-d", "--device", required=True, help="the board's REPL, e.g. COM55")
    ap.add_argument("--cycles", type=int, default=5, help="soft resets to check (default 5)")
    args = ap.parse_args()

    failed = 0

    def check(name, ok, detail):
        nonlocal failed
        print("%s  %-15s %s" % ("PASS" if ok else "FAIL", name, detail))
        failed += not ok

    for cycle in range(args.cycles + 1):
        if cycle:
            mpftp(["soft-reset"], args.device)
        values, text = run_step(args.device, BOARD_STEP)
        print("-- cycle %d%s" % (cycle, "" if cycle else " (the first program)"))
        if not values:
            check("board answered", False, text.strip()[-300:])
            continue
        fresh = cycle > 0
        if fresh:
            check("host stopped", values.get("host_running_before") == "False",
                  "running=" + str(values.get("host_running_before")))
        check("attach event", values.get("attach_event") == "True",
              "seen=" + str(values.get("attach_event")))
        moved = int(values.get("stream_bytes", "0") or 0)
        check("stream opens", moved > 0,
              "%s: %d bytes in 1 s%s" % (values.get("stream", "no stream"), moved,
                                        "; " + values["stream_error"] if "stream_error" in values else ""))

    print("GATE %s" % ("FAIL" if failed else "PASS"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
