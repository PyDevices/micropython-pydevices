# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Check that AudioSink and HandsFree play through the audio pump untouched by Python.

    <unix micropython built with audiodsp,audioif,btclassic> host_probe.py [case ...] [--fault WHICH]

Cases: ``protocol``, ``music``, ``drift``, ``call`` (all by default).
Faults: ``python`` (music: this thread reads the sink as programs did
before, and the storm must break it), ``gap`` (music: the check must see a
hole planted in the output). Each must make this exit non-zero.

The phone is `_btclassic_host`: a C thread that sends at its own clock, as
Bluedroid's task does on a board. The pump pulls on its own thread and
writes to a file at the output's pace (``pace=True``). Meanwhile this thread
runs a garbage-collection storm, with a 300 ms stall each second for the
length of a board's collection. The audio is a counter, so the output can
be checked sample by sample: every step is +1, except the one-frame drops
and repeats the clock correction makes.

``protocol``  Both profiles are audiosamples: the pump's ``info()`` reads
    their block size, and Python sees ``sample_rate``, ``channel_count`` and
    ``bits_per_sample``.
``music``     3 s of music at 44.1 kHz through a GC storm: no overrun, no
    underrun once playing, and every sample where it should be.
``drift``     The sender 5 % fast (far beyond any real clock, so 3 s shows
    it): the pull drops frames to keep up, and nothing overflows.
``call``      A 16 kHz call through a GC storm: the phone's voice comes out
    whole, and the board's microphone reaches the phone whole, sent from
    the pull by ``uplink()`` with no Python in the path.
"""

import gc
import os
import sys
import time

import _btclassic_host as host
import audiopump
import btclassic

RATE = 44100
STORM_S = 3
OUT = "/tmp/btclassic_host_probe.raw"
FAULT = None
failures = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def counter(frames, channels, down=False):
    """A counter: the first channel counts up from 0 (down from 32767 with
    `down`), and a second channel is its complement."""
    b = bytearray(frames * channels * 2)
    for i in range(frames):
        v = (0x7FFF - i if down else i) & 0x7FFF
        o = i * channels * 2
        b[o] = v & 0xFF
        b[o + 1] = v >> 8
        if channels == 2:
            w = 0x7FFF - v
            b[o + 2] = w & 0xFF
            b[o + 3] = w >> 8
    return b


# A desktop collects in a millisecond or two; a board's PSRAM heap takes
# 200 to 300 ms. So the storm also holds this thread for STALL_MS every
# STALL_EVERY_MS, the way a collection or a cover decode holds a board's.
STALL_MS = 300
STALL_EVERY_MS = 1000
_stall = [0]


def storm(seconds):
    """Garbage, collections and long stalls, as fast as this thread can make them."""
    end = time.ticks_add(time.ticks_ms(), int(seconds * 1000))
    n = 0
    junk = []
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        junk.append(bytearray(2048))
        junk.append([str(i) for i in range(50)])
        if len(junk) > 200:
            junk = []
            gc.collect()
            n += 1
        now = time.ticks_ms()
        if time.ticks_diff(now, _stall[0]) >= STALL_EVERY_MS:
            _stall[0] = now
            while time.ticks_diff(time.ticks_ms(), now) < STALL_MS:
                pass
    return n


def pump(sample, seconds):
    """Pull `sample` on the pump's thread into OUT for `seconds`, storming."""
    try:
        os.remove(OUT)
    except OSError:
        pass
    status = bytearray(audiopump.STATUS_BYTES)
    threaded = bool(audiopump.threaded())
    audiopump.spawn(sample, 1 << 30, status, sink=OUT, pace=True)
    collections = storm(seconds)
    audiopump.stop()
    audiopump.join()
    audiopump.shutdown()
    return threaded, collections


def python_fed(rx, seconds):
    """The way programs played before: this thread reads the sink into a
    Ring the pump plays, keeping 300 ms queued, and the storm holds it up."""
    try:
        os.remove(OUT)
    except OSError:
        pass
    ring = audiopump.Ring(sample_rate=RATE, channel_count=2, frames=256, capacity=RATE * 3 // 10 // 256)
    buf = bytearray(256 * 4)
    status = bytearray(audiopump.STATUS_BYTES)
    audiopump.spawn(ring, 1 << 30, status, sink=OUT, pace=True)
    end = time.ticks_add(time.ticks_ms(), int(seconds * 1000))
    n = 0
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        while ring.space() >= len(buf):
            rx.readinto(buf)
            ring.write(buf)
        n += storm(0.01)
    audiopump.stop()
    audiopump.join()
    audiopump.shutdown()
    ring.deinit()
    return bool(audiopump.threaded()), n


def to_samples(data, channels, which):
    out = []
    for o in range(which * 2, len(data) - 1, channels * 2):
        v = data[o] | (data[o + 1] << 8)
        out.append(v - 0x10000 if v & 0x8000 else v)
    return out


def read_samples(channels, which):
    with open(OUT, "rb") as f:
        return to_samples(f.read(), channels, which)


def steps(values):
    """The counter's steps once it starts: +1, drops (+2), repeats (0), and
    anything else, which is a fault."""
    i = 0
    while i < len(values) and values[i] == 0:
        i += 1      # the silence before playback starts
    plus, drop, rep, other = 0, 0, 0, []
    for k in range(i + 1, len(values)):
        d = (values[k] - values[k - 1]) & 0x7FFF
        if d == 1:
            plus += 1
        elif d == 2:
            drop += 1
        elif d == 0:
            rep += 1
        else:
            other.append((k, values[k - 1], values[k]))
    return i, plus, drop, rep, other


def case_protocol():
    bt = btclassic.Bluetooth()
    rx = btclassic.AudioSink(bt)
    hf = btclassic.HandsFree(bt, channels=1)
    info = audiopump.info(rx)
    check(info[3] == 256 * 4, "AudioSink pulls 256 stereo frames (%r)" % (info,))
    check(audiopump.info(hf)[3] == 256 * 2, "HandsFree pulls 256 mono frames")
    check((rx.channel_count, rx.bits_per_sample) == (2, 16), "AudioSink is 16-bit stereo")
    check((hf.channel_count, hf.bits_per_sample) == (1, 16), "HandsFree is 16-bit mono here")
    hf.deinit()
    rx.deinit()


def case_music(speed=1.0, label="music"):
    bt = btclassic.Bluetooth()
    rx = btclassic.AudioSink(bt, buffer_ms=500)
    before = rx.stats()
    host.stream(counter(32768, 2), RATE, speed)
    time.sleep_ms(300)          # the ring fills to its mark, as a stream starting does
    check(rx.sample_rate == RATE, "%s: the sink reports %d Hz" % (label, rx.sample_rate))
    if FAULT == "python":
        threaded, n = python_fed(rx, STORM_S)
    else:
        threaded, n = pump(rx, STORM_S)
    host.stream(None)
    after = rx.stats()
    check(threaded, "%s: the pump ran on its own thread" % label)
    print("     %d collections; ring %r" % (n, after))
    over = after["overruns"] - before["overruns"]
    under = after["underruns"] - before["underruns"]
    check(over == 0, "%s: no overruns (%d)" % (label, over))
    check(under == 0, "%s: no underruns once playing (%d)" % (label, under))
    left = read_samples(2, 0)
    if FAULT == "gap":
        mid = len(left) // 2
        left[mid:mid + 100] = [0] * 100
    lead, plus, drop, rep, other = steps(left)
    print("     %d frames out, %d silent before the start; +1 x%d, drop x%d, repeat x%d, other x%d"
          % (len(left), lead, plus, drop, rep, len(other)))
    check(len(left) > RATE * (STORM_S - 1), "%s: %d frames came out" % (label, len(left)))
    check(not other, "%s: every sample where it belongs %s" % (label, other[:3]))
    right = read_samples(2, 1)
    ok = all(left[k] + right[k] == 0x7FFF for k in range(lead, len(left)))
    check(ok, "%s: the right channel is the left's complement" % label)
    rx.deinit()
    return after["dropped"] - before["dropped"]


def case_drift():
    dropped = case_music(1.05, "drift")
    check(dropped > 0, "drift: a fast sender is caught up by dropping (%d)" % dropped)


class _Format:
    def __init__(self, rate):
        self.rate, self.channels, self.bits = rate, 1, 16


class _Mic:
    """What uplink() is handed on a board: an open pcm_in, checked by its format."""

    def __init__(self, rate):
        self.format = _Format(rate)


def case_call():
    bt = btclassic.Bluetooth()
    hf = btclassic.HandsFree(bt, channels=1, buffer_ms=400)
    host.call(16000, counter(32768, 1), counter(32768, 1, down=True))
    try:
        hf.uplink(_Mic(8000))
        check(False, "call: uplink() refuses a microphone at another rate")
    except ValueError:
        check(True, "call: uplink() refuses a microphone at another rate")
    hf.uplink(_Mic(16000))
    time.sleep_ms(300)
    threaded, n = pump(hf, STORM_S)
    st = hf.stats()
    print("     %d collections; ring %r" % (n, st))
    check(st["overruns"] == 0, "call: no overruns (%d)" % st["overruns"])
    check(st["underruns"] == 0, "call: no underruns once playing (%d)" % st["underruns"])
    lead, plus, drop, rep, other = steps(read_samples(1, 0))
    check(not other and plus > 16000 * (STORM_S - 1),
          "call: the voice came out whole (%d steps, %s)" % (plus, other[:3]))
    buf = bytearray(16000 * 2 * (STORM_S + 2))
    got = host.sent(buf)
    up = to_samples(buf[:got], 1, 0)
    breaks = [k for k in range(1, len(up)) if (up[k - 1] - up[k]) & 0x7FFF != 1]
    check(got > 16000 * 2 * (STORM_S - 1), "call: %d bytes of microphone reached the phone" % got)
    check(not breaks, "call: the microphone reached it whole (%d breaks)" % len(breaks))
    check(st["unsent"] == 0, "call: the phone's queue took everything (%d refused)" % st["unsent"])
    hf.uplink(None)
    host.call(0)
    hf.deinit()


CASES = {"protocol": case_protocol, "music": case_music, "drift": case_drift, "call": case_call}


def main(argv):
    global FAULT
    names = []
    i = 0
    while i < len(argv):
        if argv[i] == "--fault":
            FAULT = argv[i + 1]
            i += 2
        else:
            names.append(argv[i])
            i += 1
    for name in names or list(CASES):
        print("--", name)
        CASES[name]()
    print("FAILED: %d" % len(failures) if failures else "all passed")
    sys.exit(1 if failures else 0)


main(sys.argv[1:])
