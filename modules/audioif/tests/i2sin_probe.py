"""`audioi2sin.I2SIn` against what CircuitPython documents it to do.

Every leg is a behaviour taken off CircuitPython's
`shared-bindings/audioi2sin/I2SIn.c` and its espressif port, asked of this
firmware's `audioi2sin` on a desktop build. There is no I2S on a desktop, so
`record()` reads the wire frames handed in as `source=` -- two slots a frame,
at `bit_depth` -- and every conversion between the wire and the caller's
buffer is checked against the arithmetic CircuitPython does.

    <interpreter> tests/i2sin_probe.py [--fault WHICH]

Every leg prints PASS or FAIL and the last line is the tally; the exit status
is non-zero if anything failed.

`--fault rate` records a 1 kHz tone made at 8 kHz and hands it to the tone
check as 16 kHz, which is what a bus clocked at the wrong rate would do.
`--fault slot` records the right slot, which holds no tone. Either must fail
the tone leg, or that leg is not a check.
"""

import array
import math
import sys

import audioi2sin

try:
    import audiometer  # noqa: F401
    from tone_check import report, tone_check
except ImportError:
    tone_check = None

FAULT = None
if "--fault" in sys.argv:
    FAULT = sys.argv[sys.argv.index("--fault") + 1]
FAILS = []


def check(name, ok, detail=""):
    print("%-48s %s %s" % (name, "PASS" if ok else "FAIL", detail))
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


def widen(s, i, o):
    """CircuitPython's shared_audioi2sin_write_converted, in Python."""
    if o > i:
        top = (s << (32 - i)) & 0xFFFFFFFF
        top |= top >> i
        if i * 2 < o:
            top |= top >> (i * 2)
        if top & 0x80000000:
            top -= 1 << 32
        return top >> (32 - o)
    if o == i:
        return s
    return s >> (i - o)


PINS = (12, 10, 11)
N = 64
L16 = [((i * 977) % 65536) - 32768 for i in range(N)]
R16 = [-v - 1 for v in L16]
SRC16 = wire(L16, R16)

# --- the constructor ------------------------------------------------------
mic = audioi2sin.I2SIn(*PINS, source=SRC16)
check("defaults: 16 kHz, 16-bit, mono, signed",
      (mic.sample_rate, mic.bit_depth, mic.bits_per_sample, mic.channel_count,
       mic.samples_signed) == (16000, 16, 16, 1, True))
check("overflow starts False", mic.overflow is False)
mic.deinit()

why = raises(ValueError, lambda: audioi2sin.I2SIn(*PINS, bit_depth=12))
check("bit_depth=12 is refused", why is not None and "bit_depth" in why, why)
why = raises(ValueError, lambda: audioi2sin.I2SIn(*PINS, output_bit_depth=20))
check("output_bit_depth=20 is refused", why is not None, why)
why = raises(NotImplementedError,
             lambda: audioi2sin.I2SIn(*PINS, external_clock=True))
check("external_clock raises NotImplementedError", why is not None, why)

# --- mono is the left slot, stereo is both --------------------------------
buf = array.array("h", [0] * N)
with audioi2sin.I2SIn(*PINS, source=SRC16) as mic:
    n = mic.record(buf, N)
check("mono 16-bit: the left slot, sample for sample",
      n == N and list(buf) == L16, "n=%d" % n)

buf = array.array("h", [0] * (2 * N))
with audioi2sin.I2SIn(*PINS, mono=False, source=SRC16) as mic:
    n = mic.record(buf, 2 * N)
    cc = mic.channel_count
want = []
for i in range(N):
    want += [L16[i], R16[i]]
check("stereo 16-bit: L, R interleaved", n == 2 * N and list(buf) == want
      and cc == 2)

# An odd count in stereo ends on a left sample, as CircuitPython's does.
buf = array.array("h", [0] * 5)
with audioi2sin.I2SIn(*PINS, mono=False, source=SRC16) as mic:
    n = mic.record(buf, 5)
check("stereo, odd count: five samples, L R L R L",
      n == 5 and list(buf) == [L16[0], R16[0], L16[1], R16[1], L16[2]])

# --- record() is continuous across calls ----------------------------------
a = array.array("h", [0] * 10)
b = array.array("h", [0] * 10)
with audioi2sin.I2SIn(*PINS, source=SRC16) as mic:
    mic.record(a, 10)
    mic.record(b, 10)
check("two record() calls continue where the first stopped",
      list(a) + list(b) == L16[:20])

# --- the conversions ------------------------------------------------------
buf = array.array("H", [0] * N)
with audioi2sin.I2SIn(*PINS, samples_signed=False, source=SRC16) as mic:
    mic.record(buf, N)
check("unsigned 16: the sign bit flipped",
      list(buf) == [(v ^ 0x8000) & 0xFFFF for v in [x & 0xFFFF for x in L16]])

buf = array.array("b", [0] * N)
with audioi2sin.I2SIn(*PINS, output_bit_depth=8, source=SRC16) as mic:
    mic.record(buf, N)
    bps = mic.bits_per_sample
check("16 -> 8: arithmetic shift right by 8",
      list(buf) == [v >> 8 for v in L16] and bps == 8)

buf = array.array("i", [0] * N)
with audioi2sin.I2SIn(*PINS, output_bit_depth=32, source=SRC16) as mic:
    mic.record(buf, N)
check("16 -> 32: bit-replicated, full scale stays full scale",
      list(buf) == [widen(v, 16, 32) for v in L16])
check("16 -> 32: 0x7FFF becomes 0x7FFF7FFF", widen(0x7FFF, 16, 32) == 0x7FFF7FFF)

# 24-bit microphones put the sample in the top 24 bits of a 32-bit slot.
L24 = [((i * 104729) % (1 << 24)) - (1 << 23) for i in range(N)]
SRC32 = wire([v << 8 for v in L24], [0] * N, "i")
buf = array.array("i", [0] * N)
with audioi2sin.I2SIn(*PINS, bit_depth=24, source=SRC32) as mic:
    mic.record(buf, N)
check("24-bit in a 32-bit slot: the top 24 bits, sign-extended",
      list(buf) == L24)

buf = array.array("h", [0] * N)
with audioi2sin.I2SIn(*PINS, bit_depth=32, output_bit_depth=16,
                      source=SRC32) as mic:
    mic.record(buf, N)
check("32 -> 16: the top 16 bits", list(buf) == [(v << 8) >> 16 for v in L24])

# --- what record() refuses -------------------------------------------------
mic = audioi2sin.I2SIn(*PINS, source=SRC16)
why = raises(TypeError, lambda: mic.record(array.array("i", [0] * N), N))
check("a 32-bit buffer for 16-bit samples is refused",
      why is not None and "h" in why, why)
why = raises(ValueError, lambda: mic.record(array.array("h", [0] * 4), 5))
check("a buffer shorter than the count is refused", why is not None, why)
check("a bytearray takes 8-bit samples",
      audioi2sin.I2SIn(*PINS, output_bit_depth=8, source=SRC16).record(
          bytearray(8), 8) == 8)
mic.deinit()
why = raises(ValueError, lambda: mic.record(array.array("h", [0] * 4), 4))
check("record() after deinit() raises", why is not None, why)
mic.deinit()
check("deinit() twice is harmless", True)

# --- the docstring example's shape, with no source: silence, paced --------
buf = array.array("h", [1] * 1600)
with audioi2sin.I2SIn(*PINS, sample_rate=16000, bit_depth=16) as mic:
    n = mic.record(buf, len(buf))
check("no source: the count recorded, all silence",
      n == 1600 and max(buf) == 0 and min(buf) == 0)

# --- a 1 kHz tone, through audiometer --------------------------------------
if tone_check is None:
    print("audiometer is not in this build: the tone leg is skipped")
else:
    made_at = 8000 if FAULT == "rate" else 16000
    frames = made_at // 2
    tone = [int(12000 * math.sin(2 * math.pi * 1000 * i / made_at))
            for i in range(frames)]
    hiss = [((i * 7919) % 201) - 100 for i in range(frames)]
    src = wire(tone, hiss)
    buf = array.array("h", [0] * 16000)
    with audioi2sin.I2SIn(*PINS, mono=True, sample_rate=16000,
                          source=src) as mic:
        if FAULT == "slot":
            # The right slot, where there is no tone.
            both = array.array("h", [0] * 32000)
            mic.deinit()
            with audioi2sin.I2SIn(*PINS, mono=False, source=src) as m2:
                m2.record(both, 32000)
            for i in range(16000):
                buf[i] = both[2 * i + 1]
        else:
            mic.record(buf, len(buf))
    r = tone_check(buf, 16000, hz=1000)
    check("a 1 kHz tone shows at 1 kHz through audiometer", r["ok"], report(r))

print()
print("%d failed" % len(FAILS) if FAILS else "all passed")
sys.exit(1 if FAILS else 0)
