# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
#
# audiobusio.PDMIn on an ESP32 with a PDM microphone: which slot it is on, a
# 16 kHz recording with its level, a second recording, the errors, and that
# deinit gives the pins back. Pins default to the T-Watch S3's microphone
# (clock 44, data 47); `CLK`/`DAT` override them.
#
#     mcuctl run pdmin_probe.py
#
# The recording is left in /pdm_cap.raw (signed 16-bit little-endian, 16 kHz)
# for `mcuctl get`. Play a tone near the microphone while it runs.

import array
import machine
import audiobusio

CLK = globals().get("CLK", 44)
DAT = globals().get("DAT", 47)
RATE = 16000


def stats(a, lo=0, step=1):
    n = 0
    s = 0
    s2 = 0
    mn = 32767
    mx = -32768
    clip = 0
    for i in range(lo, len(a), step):
        v = a[i]
        n += 1
        s += v
        s2 += v * v
        if v < mn:
            mn = v
        if v > mx:
            mx = v
        if v <= -32767 or v >= 32767:
            clip += 1
    mean = s / n
    var = s2 / n - mean * mean
    return {"n": n, "mean": round(mean), "std": round(max(var, 0) ** 0.5), "min": mn, "max": mx, "clip": clip}


def raises(exc, fn):
    try:
        fn()
    except exc:
        return True
    except Exception as e:
        return "wrong %r" % (e,)
    return "no error"


# 1. which slot carries the microphone
with audiobusio.PDMIn(CLK, DAT, sample_rate=RATE, bit_depth=16, mono=False) as mic:
    st = array.array("h", [0] * (RATE * 2))
    got = mic.record(st, len(st))
    print("@@ stereo got", got, "slot0", stats(st, 0, 2), "slot1", stats(st, 1, 2))

# 2. mono, three seconds, saved
with audiobusio.PDMIn(CLK, DAT, sample_rate=RATE, bit_depth=16) as mic:
    print("@@ rate", mic.sample_rate)
    buf = array.array("h", [0] * (RATE * 3))
    got = mic.record(buf, len(buf))
    print("@@ mono got", got, stats(buf))
    with open("/pdm_cap.raw", "wb") as f:
        f.write(buf)
    # 3. a second record on the same object
    got2 = mic.record(buf, RATE)
    print("@@ second got", got2, stats(buf[:RATE]))
    ub = array.array("H", [0] * 1000)
    mic.record(ub, 1000)
    print("@@ H rests near 32768:", sum(ub) // 1000)

# 4. errors
print("@@ bit_depth 12", raises(ValueError, lambda: audiobusio.PDMIn(CLK, DAT, bit_depth=12)))
print("@@ oversample 7", raises(ValueError, lambda: audiobusio.PDMIn(CLK, DAT, oversample=7)))
print("@@ startup 2", raises(ValueError, lambda: audiobusio.PDMIn(CLK, DAT, startup_delay=2)))
print("@@ rate 0", raises(ValueError, lambda: audiobusio.PDMIn(CLK, DAT, sample_rate=0)))
m = audiobusio.PDMIn(CLK, DAT, sample_rate=RATE, bit_depth=16, startup_delay=0)
print("@@ second open", raises(RuntimeError, lambda: audiobusio.PDMIn(CLK, DAT, bit_depth=16)))
print("@@ wrong buf", raises(ValueError, lambda: m.record(bytearray(10), 10)))
print("@@ too small", raises(ValueError, lambda: m.record(array.array("H", [0] * 4), 10)))
m.deinit()
m.deinit()
print("@@ deinited record", raises(ValueError, lambda: m.record(array.array("H", [0] * 4), 4)))
print("@@ deinited rate", raises(ValueError, lambda: m.sample_rate))

# 5. deinit gave the pins back: a Pin takes them, then another PDMIn opens
p = machine.Pin(CLK, machine.Pin.OUT)
p.value(1)
print("@@ pin free", p.value() == 1)
del p
with audiobusio.PDMIn(CLK, DAT, sample_rate=RATE, bit_depth=8) as mic:
    b8 = bytearray(2000)
    n8 = mic.record(b8, len(b8))
    print("@@ reopen 8-bit got", n8, "mean", sum(b8) // len(b8))
print("@@ done")
