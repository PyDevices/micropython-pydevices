"""`audiobusio.I2SOut` against what CircuitPython documents it to do.

Everything here is a CircuitPython behaviour taken off
`shared-bindings/audiobusio/I2SOut.c` and
`ports/espressif/common-hal/audiobusio/__init__.c`, asked of this firmware's
`audiobusio`. On a desktop the bus is the driver's file sink, paced off the
wall clock, so `playing`, `pause`, `resume` and the end of a sample mean the
same thing here as they do on a board.

    <interpreter> tests/busio_probe.py [scratch-dir] [--fault WHICH]

Every leg prints PASS or FAIL and the last line is the tally; the exit
status is non-zero if anything failed.

Needs a build carrying audiodsp's usermod AND this repository's, which is
what `.github/workflows/build.yml` makes. On a desktop the bus is this
driver's file sink, paced off the wall clock, so `playing`, `pause`,
`resume` and the end of a sample mean here what they mean on a board --
and the file is the only reason any of it is checkable without ears.

`--fault deaf` makes every byte count read as zero. Nearly every leg here
asks "did the bytes come out", so if the run still passes with the sink
reading empty, the legs are not reading anything and the whole file is
decoration. It is the one plant that cannot be satisfied by accident.
"""

import array
import math
import os
import struct
import sys
import time

import audiobusio
import audiocore
import audiomixer
import audiopump

try:
    import synthio
except ImportError:
    synthio = None

_ARGS = sys.argv[1:]
FAULT = None
if "--fault" in _ARGS:
    _AT = _ARGS.index("--fault")
    FAULT = _ARGS[_AT + 1]
    del _ARGS[_AT:_AT + 2]
DIR = _ARGS[0] if _ARGS else os.getenv("PUMP_PROBE_TMP", "/tmp")
RATE = 24000
FAILS = []


def check(name, ok, detail=""):
    print("%-44s %s %s" % (name, "PASS" if ok else "FAIL", detail))
    if not ok:
        FAILS.append(name)


def sink(n):
    return "%s/busio_%s.raw" % (DIR, n)


def size(path):
    if FAULT == "deaf":
        return 0
    try:
        return os.stat(path)[6]
    except OSError:
        return 0


def _tone(hz, rate=RATE, channels=2, signed=True, bits=16, level=0.5):
    length = max(2, int(rate // hz))
    if bits == 16:
        code = "h" if signed else "H"
    else:
        code = "b" if signed else "B"
    buf = array.array(code, [0] * (length * channels))
    peak = int(((1 << (bits - 1)) - 1) * level)
    bias = 0 if signed else (1 << (bits - 1))
    for i in range(length):
        v = int(math.sin(math.pi * 2 * i / length) * peak) + bias
        for c in range(channels):
            buf[i * channels + c] = v
    return buf


def raw(hz=220, rate=RATE, channels=2, signed=True, bits=16, level=0.5):
    return audiocore.RawSample(_tone(hz, rate, channels, signed, bits, level),
                               sample_rate=rate, channel_count=channels)


def wav(path, hz=220, seconds=0.4, rate=RATE, channels=2, level=0.5):
    """A real RIFF file on the filesystem, for the WaveFile leg."""
    frames = int(rate * seconds)
    period = max(2, int(rate // hz))
    peak = int(32767 * level)
    data = array.array("h", [0] * (frames * channels))
    for i in range(frames):
        v = int(math.sin(math.pi * 2 * (i % period) / period) * peak)
        for c in range(channels):
            data[i * channels + c] = v
    body = bytes(memoryview(data))
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate,
                                      rate * channels * 2, channels * 2, 16))
        f.write(b"data" + struct.pack("<I", len(body)) + body)
    return path


# --- 1. the constructor ---------------------------------------------------

def leg_construct():
    out = audiobusio.I2SOut(1, 0, 9)
    check("construct: takes three pins", True)
    try:
        audiobusio.I2SOut(4, 5, 6)
        check("construct: a second one is refused", False, "it was allowed")
    except RuntimeError as exc:
        check("construct: a second one is refused", "in use" in str(exc),
              repr(str(exc)))
    out.deinit()
    second = audiobusio.I2SOut(4, 5, 6)
    check("construct: allowed again after deinit", True)
    second.deinit()

    try:
        audiobusio.I2SOut(1, 0, 9, external_clock=True)
        check("construct: external_clock says NotImplemented", False)
    except NotImplementedError:
        check("construct: external_clock says NotImplemented", True)

    with audiobusio.I2SOut(1, 0, 9) as ctx:
        inside = ctx.playing is False
    try:
        ctx.playing
        after = False
    except ValueError:
        after = True
    check("construct: context manager deinits on exit", inside and after)

    dead = audiobusio.I2SOut(1, 0, 9)
    dead.deinit()
    try:
        dead.play(raw())
        check("deinited: every method raises ValueError", False)
    except ValueError:
        check("deinited: every method raises ValueError", True)
    dead.deinit()          # CircuitPython's deinit is idempotent
    check("deinited: deinit twice is fine", True)


# --- 2. play, stop, playing ----------------------------------------------

def leg_play():
    path = sink("play")
    out = audiobusio.I2SOut(1, 0, 9, sink=path)
    sample = raw(220)
    out.play(sample, loop=True)
    check("play: playing goes True", out.playing is True)
    time.sleep(0.3)
    out.stop()
    check("play: stop makes playing False", out.playing is False)
    n = size(path)
    want = int(RATE * 0.3) * 4
    check("play: a third of a second of audio came out",
          abs(n - want) < want // 3, "%d bytes, wanted about %d" % (n, want))

    try:
        out.play(b"not a sample")
        check("play: a non-sample raises TypeError", False)
    except TypeError:
        check("play: a non-sample raises TypeError", True)

    # CircuitPython's I2SOut.play stops whatever was playing first.
    out.play(raw(220), loop=True)
    out.play(raw(330), loop=True)
    check("play: play while playing replaces it", out.playing is True)
    out.stop()
    out.deinit()


# --- 3. loop, and the end of a non-looping sample -------------------------

def leg_loop():
    path = sink("loop")
    out = audiobusio.I2SOut(1, 0, 9, sink=path)
    out.play(raw(220), loop=True)
    time.sleep(0.4)
    looped = size(path)
    out.stop()
    out.deinit()

    path2 = sink("once")
    out = audiobusio.I2SOut(1, 0, 9, sink=path2)
    once = raw(220)
    out.play(once, loop=False)
    deadline = time.time() + 2.0
    while out.playing and time.time() < deadline:
        pass
    ended = not out.playing
    once_bytes = size(path2)
    out.deinit()

    check("loop=True: it keeps going", looped > RATE * 4 // 4,
          "%d bytes in 0.4 s" % looped)
    check("loop=False: playing goes False on its own", ended)
    check("loop=False: it stopped after about one buffer",
          0 < once_bytes <= looped // 4, "%d bytes" % once_bytes)


# --- 4. pause, resume, paused --------------------------------------------

def leg_pause():
    path = sink("pause")
    out = audiobusio.I2SOut(1, 0, 9, sink=path)
    out.play(raw(220), loop=True)
    time.sleep(0.2)
    out.pause()
    check("pause: paused goes True", out.paused is True)
    at_pause = size(path)
    time.sleep(0.3)
    while_paused = size(path) - at_pause
    check("pause: nothing is written while paused", while_paused == 0,
          "%d bytes" % while_paused)
    check("pause: playing stays True while paused", out.playing is True)
    out.resume()
    check("resume: paused goes False", out.paused is False)
    time.sleep(0.2)
    after = size(path) - at_pause
    check("resume: it plays again", after > 0, "%d bytes" % after)
    out.stop()

    try:
        out.pause()
        check("pause: raises RuntimeError when not playing", False)
    except RuntimeError as exc:
        check("pause: raises RuntimeError when not playing",
              "Not playing" in str(exc), repr(str(exc)))
    out.deinit()


# --- 5. the graphs CircuitPython code actually plays ----------------------

def leg_graphs():
    path = sink("mixer")
    out = audiobusio.I2SOut(1, 0, 9, sink=path)
    mixer = audiomixer.Mixer(voice_count=2, sample_rate=RATE, channel_count=2,
                             bits_per_sample=16, samples_signed=True,
                             buffer_size=2048)
    out.play(mixer)
    mixer.voice[0].level = 0.5
    mixer.voice[1].level = 0.5
    mixer.play(raw(220), voice=0, loop=True)
    mixer.play(raw(330), voice=1, loop=True)
    time.sleep(0.3)
    n = size(path)
    out.stop()
    out.deinit()
    check("Mixer: plays, two voices", n > RATE, "%d bytes" % n)

    if synthio is None:
        check("synthio.Synthesizer: plays", False, "no synthio in this build")
        return
    path = sink("synth")
    out = audiobusio.I2SOut(1, 0, 9, sink=path)
    synth = synthio.Synthesizer(sample_rate=RATE, channel_count=2)
    out.play(synth)
    synth.press((60, 64, 67))
    time.sleep(0.3)
    n = size(path)
    synth.release_all()
    out.stop()
    out.deinit()
    check("synthio.Synthesizer: plays a chord", n > RATE, "%d bytes" % n)


# --- 6. a WAV off the filesystem -----------------------------------------

def leg_wav():
    src = wav("%s/busio_tone.wav" % DIR, seconds=0.4)
    path = sink("wav")
    out = audiobusio.I2SOut(1, 0, 9, sink=path)
    f = open(src, "rb")
    w = audiocore.WaveFile(f)
    out.play(w)
    deadline = time.time() + 4.0
    while out.playing and time.time() < deadline:
        pass
    ended = not out.playing
    n = size(path)
    out.deinit()
    f.close()
    want = int(RATE * 0.4) * 4
    check("WaveFile: i2s.play(audiocore.WaveFile(f)) works", n > 0,
          "%d bytes" % n)
    check("WaveFile: playing goes False at the end of the file", ended)
    check("WaveFile: the whole file came out",
          abs(n - want) <= want // 8, "%d bytes, the file is %d" % (n, want))

    # ...and looped, which is the other half of what the output does.
    path = sink("wavloop")
    out = audiobusio.I2SOut(1, 0, 9, sink=path)
    f = open(src, "rb")
    out.play(audiocore.WaveFile(f), loop=True)
    time.sleep(1.0)
    n = size(path)
    out.stop()
    out.deinit()
    f.close()
    check("WaveFile: loop=True plays past the end of the file",
          n > want + want // 2, "%d bytes against a %d-byte file" % (n, want))


# --- 7. fifty times round, and the REPL still there -----------------------

def leg_storm():
    path = sink("storm")
    out = audiobusio.I2SOut(1, 0, 9, sink=path)
    sample = raw(220)
    bad = 0
    for i in range(50):
        out.play(sample, loop=True)
        if not out.playing:
            bad += 1
        time.sleep(0.01)
        out.stop()
        if out.playing:
            bad += 1
    check("50 x play/stop: every one took", bad == 0, "%d bad" % bad)
    check("50 x play/stop: the pump is gone at the end",
          audiopump.running() is False)
    check("50 x play/stop: no starved blocks", out.starved() == 0,
          "%d" % out.starved())
    out.deinit()
    check("50 x play/stop: the interpreter is still here", 2 + 2 == 4)


# --- 8. the three that are ours, not CircuitPython's ----------------------

def leg_ours():
    """`retarget`, `status` and `main_clock_fs`.

    CircuitPython has none of them and does not need them: one output, one
    graph, and `play()` again is a fine way to start a different sound. A
    POLICY layer over this class -- `audiodev`, which arbitrates two clients
    through a root mixer -- has a new tail every time a client arrives or
    leaves, and going through `play()` there is a stop, a join and a fresh
    channel between two blocks a listener is in the middle of.
    """
    path = sink("ours")
    out = audiobusio.I2SOut(1, 0, 9, sink=path, main_clock_fs=384)
    check("ours: main_clock_fs is taken", True, "constructed")

    check("ours: status is the pump's block",
          len(out.status) == audiopump.STATUS_BYTES,
          "%d bytes" % len(out.status))

    try:
        out.retarget(raw(330))
        check("ours: retarget on a stopped output raises", False)
    except RuntimeError as exc:
        check("ours: retarget on a stopped output raises",
              "Not playing" in str(exc), str(exc))

    out.play(raw(220), loop=True)
    time.sleep(0.2)
    before = size(path)
    mixer = audiomixer.Mixer(voice_count=1, sample_rate=RATE, channel_count=2,
                             bits_per_sample=16, samples_signed=True,
                             buffer_size=2048)
    mixer.play(raw(330), voice=0, loop=True)
    out.retarget(mixer, loop=True)
    time.sleep(0.3)
    after = size(path)
    check("ours: it plays on through the swap", out.playing is True)
    check("ours: and the bytes kept coming", after > before + 10000,
          "%d bytes over the swap" % (after - before))
    check("ours: the file was not reopened", after > before,
          "%d then %d" % (before, after))

    try:
        out.retarget(b"not a sample")
        check("ours: retarget refuses a non-sample", False)
    except TypeError:
        check("ours: retarget refuses a non-sample", True)

    out.stop()
    out.deinit()


for leg in (leg_construct, leg_play, leg_loop, leg_pause, leg_graphs,
            leg_wav, leg_storm, leg_ours):
    print("-- %s" % leg.__name__)
    leg()

print("")
if FAILS:
    print("FAILED %d: %s" % (len(FAILS), ", ".join(FAILS)))
    sys.exit(1)
print("all legs PASS")
sys.exit(0)
