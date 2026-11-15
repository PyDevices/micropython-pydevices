# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""`audioi2sin.I2SIn(take=)`, and `I2SIn` played as an audiosample.

    <interpreter> tests/i2sin_play_probe.py [scratch-dir] [--fault WHICH]

On a desktop build `I2SIn` reads the two-slot wire frames handed in as
`source=`. Each leg here plays one through `audiobusio.I2SOut`'s file sink,
or records it, and compares the samples that came out with the slot `take`
asked for, sample for sample.

Every leg prints PASS or FAIL and the last line is the tally; the exit status
is non-zero if anything failed.

`--fault take` compares the right slot's leg against the left slot. `--fault
short` cuts the sink's file to a few samples before it is read. Either must
fail, or the legs they touch are not checks.
"""

import array
import os
import sys
import time

import audiobusio
import audioi2sin
import audiomixer

_ARGS = sys.argv[1:]
FAULT = None
if "--fault" in _ARGS:
    _AT = _ARGS.index("--fault")
    FAULT = _ARGS[_AT + 1]
    del _ARGS[_AT:_AT + 2]
DIR = _ARGS[0] if _ARGS else os.getenv("PUMP_PROBE_TMP", "/tmp")
FAILS = []

PINS = (12, 10, 11)
RATE = 16000
# Not a multiple of the pump's block, so the legs see the source wrap
# mid-block.
N = 100
L16 = [((i * 977) % 65536) - 32768 for i in range(N)]
R16 = [((i * 7919) % 30001) - 15000 for i in range(N)]


def check(name, ok, detail=""):
    print("%-52s %s %s" % (name, "PASS" if ok else "FAIL", detail))
    if not ok:
        FAILS.append(name)


def raises(exc, fn):
    try:
        fn()
    except exc as e:
        return str(e)
    except Exception as e:  # noqa: BLE001
        return "wrong exception %r" % e
    return None


def wire(left, right, code="h"):
    """Interleave two slot lists into the bytes the DMA would hold."""
    out = array.array(code, [0] * (2 * len(left)))
    for i in range(len(left)):
        out[2 * i] = left[i]
        out[2 * i + 1] = right[i]
    return bytes(out)


SRC16 = wire(L16, R16)
MIX16 = [(L16[i] + R16[i]) >> 1 for i in range(N)]


def looped(slot, count):
    return [slot[i % len(slot)] for i in range(count)]


def play(source, seconds=0.25, name="play"):
    """Play `source` through I2SOut's file sink and give back its samples."""
    path = "%s/i2sin_%s.raw" % (DIR, name)
    out = audiobusio.I2SOut(1, 0, 9, sink=path)
    out.play(source)
    time.sleep(seconds)
    playing = out.playing
    out.stop()
    out.deinit()
    with open(path, "rb") as f:
        data = f.read()
    if FAULT == "short":
        data = data[:8]
    return playing, array.array("h", data)


# --- take= on record() -----------------------------------------------------

for take, want, label in ((0, L16, "left slot"), (1, R16, "right slot"),
                          (2, MIX16, "average of the two")):
    buf = array.array("h", [0] * N)
    with audioi2sin.I2SIn(*PINS, take=take, source=SRC16) as mic:
        n = mic.record(buf, N)
        cc = mic.channel_count
    check("record, take=%d: the %s" % (take, label),
          n == N and cc == 1 and list(buf) == want)

buf = array.array("h", [0] * (2 * N))
with audioi2sin.I2SIn(*PINS, mono=False, take=3, source=SRC16) as mic:
    mic.record(buf, 2 * N)
check("record, take=3: both slots", list(buf) == list(array.array("h", SRC16)))

# The mix of two full-scale 32-bit slots must not overflow.
SRC32 = wire([0x7FFFFF00, -0x80000000], [0x7FFFFF00, -0x80000000], "i")
buf = array.array("i", [0] * 2)
with audioi2sin.I2SIn(*PINS, bit_depth=32, take=2, source=SRC32) as mic:
    mic.record(buf, 2)
check("record, take=2 at 32 bits: full scale stays full scale",
      list(buf) == [0x7FFFFF00, -0x80000000], str(list(buf)))

why = raises(ValueError, lambda: audioi2sin.I2SIn(*PINS, take=4))
check("take=4 is refused", why is not None and "take" in why, why)
why = raises(ValueError, lambda: audioi2sin.I2SIn(*PINS, take=3))
check("take=3 with mono=True is refused", why is not None and "mono" in why,
      why)
why = raises(ValueError, lambda: audioi2sin.I2SIn(*PINS, mono=False, take=1))
check("take=1 with mono=False is refused", why is not None and "mono" in why,
      why)

# --- I2SOut.play(mic) ------------------------------------------------------

mic = audioi2sin.I2SIn(*PINS, sample_rate=RATE, source=SRC16)
check("an I2SIn is an audiosample: 16 kHz, 16-bit, mono",
      (mic.sample_rate, mic.bits_per_sample, mic.channel_count)
      == (RATE, 16, 1))
playing, got = play(mic, name="left")
mic.deinit()
check("play(mic): still playing, a live mic never ends", playing)
check("play(mic): about a quarter second came out",
      abs(len(got) - RATE // 4) < RATE // 12, "%d samples" % len(got))
check("play(mic): the left slot, sample for sample",
      len(got) > 1000 and list(got) == looped(L16, len(got)))

mic = audioi2sin.I2SIn(*PINS, sample_rate=RATE, take=1, source=SRC16)
playing, got = play(mic, name="right")
mic.deinit()
want = L16 if FAULT == "take" else R16
check("play(mic), take=1: the right slot",
      len(got) > 1000 and list(got) == looped(want, len(got)))

mic = audioi2sin.I2SIn(*PINS, sample_rate=RATE, take=2, source=SRC16)
playing, got = play(mic, name="mix")
mic.deinit()
check("play(mic), take=2: the average",
      len(got) > 1000 and list(got) == looped(MIX16, len(got)))

mic = audioi2sin.I2SIn(*PINS, sample_rate=RATE, mono=False, source=SRC16)
playing, got = play(mic, name="stereo")
mic.deinit()
both = list(array.array("h", SRC16))
check("play(mic), stereo: L and R interleaved",
      len(got) > 2000 and list(got) == looped(both, len(got)))

# Unsigned samples take the output's conversion back to signed 16-bit
# stereo; what comes out is the left slot again, in both channels.
mic = audioi2sin.I2SIn(*PINS, sample_rate=RATE, samples_signed=False,
                       source=SRC16)
playing, got = play(mic, name="unsigned")
mic.deinit()
frames = len(got) // 2
check("play(mic), unsigned: converted back, left in both channels",
      frames > 1000
      and [got[2 * i] for i in range(frames)] == looped(L16, frames)
      and [got[2 * i + 1] for i in range(frames)] == looped(L16, frames))

# A 24-bit microphone plays at 16 bits: the top 16 of its 24.
L24 = [((i * 104729) % (1 << 24)) - (1 << 23) for i in range(N)]
mic = audioi2sin.I2SIn(*PINS, sample_rate=RATE, bit_depth=24,
                       source=wire([v << 8 for v in L24], [0] * N, "i"))
check("a 24-bit mic plays at 16 bits", mic.bits_per_sample == 24)
playing, got = play(mic, name="24bit")
mic.deinit()
check("play(mic), 24-bit: the top 16 bits",
      len(got) > 1000 and list(got) == looped([v >> 8 for v in L24], len(got)))

# Through a mixer voice at full level, the samples come out unchanged.
mic = audioi2sin.I2SIn(*PINS, sample_rate=RATE, source=SRC16)
mixer = audiomixer.Mixer(voice_count=1, sample_rate=RATE, channel_count=1,
                         bits_per_sample=16, samples_signed=True)
mixer.voice[0].level = 1.0
mixer.voice[0].play(mic)
playing, got = play(mixer, name="mixer")
mixer.deinit()
mic.deinit()
# A mixer opens with a few samples of silence, a RawSample's voice as much
# as this one, so the stream is matched from its first sound. No value in L16
# is zero, and each is distinct, which says where in the loop it starts.
lead = 0
while lead < len(got) and got[lead] == 0:
    lead += 1
got = got[lead:]
at = L16.index(got[0]) if len(got) and got[0] in L16 else 0
check("a mixer voice plays the mic, sample for sample",
      lead < 512 and len(got) > 1000
      and list(got) == looped(L16[at:] + L16[:at], len(got)),
      "%d samples of silence first" % lead)

# deinit() while it plays: the output goes on, in silence, and nothing hangs.
mic = audioi2sin.I2SIn(*PINS, sample_rate=RATE, source=SRC16)
path = "%s/i2sin_deinit.raw" % DIR
out = audiobusio.I2SOut(1, 0, 9, sink=path)
out.play(mic)
time.sleep(0.1)
mic.deinit()
time.sleep(0.1)
out.stop()
out.deinit()
check("deinit() while playing returns and the output stops", True)

print()
print("%d failed" % len(FAILS) if FAILS else "all passed")
sys.exit(1 if FAILS else 0)
