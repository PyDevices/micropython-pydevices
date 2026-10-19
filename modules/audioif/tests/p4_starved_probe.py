"""`I2SOut.starved()` on a board: 0 while nothing starves, and it moves when
the pump really falls behind.

    mpftp put -d COM4 tests/p4_starved_probe.py /p4_starved_probe.py
    mpftp exec -d COM4 "import p4_starved_probe as p; p.run()"

The pins are the Waveshare ESP32-P4 Touch-LCD panel's (ES8311 on I2S 0); pass
others to `run()` for another board. Nothing is powered on the codec side, so
the probe is silent: the count is the DMA's business, not the speaker's.

Every leg prints PASS or FAIL and the last line is the tally.

The planted fault is `audiopump.park()`: the pump stops handing the DMA
anything for 200 ms while `I2SOut` still thinks it is playing, which is
exactly a pump that fell behind. If `starved()` stays near 0 through that,
every other leg's 0 means nothing. A pause is the opposite case, silence that
was asked for, and must not be counted.
"""

import array
import gc
import math
import time

import _audioif
import audiobusio
import audiocore
import audiopump

RATE = 24000
FRAME = 4               # 16-bit stereo on the wire
FAILS = []


def check(name, ok, detail=""):
    print("%-48s %s %s" % (name, "PASS" if ok else "FAIL", detail))
    if not ok:
        FAILS.append(name)


def tone(hz=1000, rate=RATE):
    n = rate // hz
    buf = array.array("h", [0] * (2 * n))
    for i in range(n):
        v = int(3000 * math.sin(2 * math.pi * i / n))
        buf[2 * i] = v
        buf[2 * i + 1] = v
    return audiocore.RawSample(buf, sample_rate=rate, channel_count=2)


def watch(out, seconds, step_ms=100, between=None):
    """starved() every step: (all readings, whether it ever went down)."""
    seen = []
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < seconds * 1000:
        if between is not None:
            between()
        seen.append(out.starved())
        time.sleep_ms(step_ms)
    fell = any(b < a for a, b in zip(seen, seen[1:]))
    return seen, fell


def run(bclk=12, ws=10, data=9, mclk=13, din=11):
    # No port named: the driver picks a free one, as CircuitPython's does.
    out = audiobusio.I2SOut(bclk, ws, data, main_clock=mclk)
    try:
        out.play(tone(), loop=True)
        seen, fell = watch(out, 3.0)
        check("clean playback: starved() stays 0", max(seen) == 0,
              "max %d" % max(seen))
        check("clean playback: never goes down", not fell)

        seen, fell = watch(out, 1.0, between=gc.collect)
        check("gc.collect() between reads: still 0", max(seen) == 0,
              "max %d" % max(seen))

        before = out.starved()
        audiopump.park(200000)
        time.sleep_ms(200)
        audiopump.unpark()
        time.sleep_ms(100)
        planted = out.starved() - before
        lo, hi = RATE * FRAME * 150 // 1000, RATE * FRAME * 260 // 1000
        check("planted 200 ms park: counted", lo <= planted <= hi,
              "%d bytes, %d ms" % (planted, planted * 1000 // (RATE * FRAME)))
        seen, fell = watch(out, 1.0, between=gc.collect)
        check("after the park: never goes down", not fell and seen[0] >= planted,
              "%d .. %d" % (seen[0], seen[-1]))
        check("after the park: stops growing", seen[-1] == seen[0],
              "%d -> %d" % (seen[0], seen[-1]))

        held = out.starved()
        out.pause()
        time.sleep_ms(300)
        out.resume()
        time.sleep_ms(300)
        check("a pause is not counted, and the count survives it",
              out.starved() == held, "%d -> %d" % (held, out.starved()))

        out.play(tone(), loop=True)
        time.sleep(1)
        check("play() again starts from 0", out.starved() == 0,
              "%d" % out.starved())

        # A recorder on the same channel makes the ring run shallower; the old
        # count read tens to hundreds of bytes here on a clean playback.
        out.stop()
        out.deinit()
        out = audiobusio.I2SOut(bclk, ws, data, main_clock=mclk, data_in=din)
        out.play(tone(), loop=True)
        _audioif.rx_open(0, bclk, ws, data, din, RATE, mclk=mclk, ring=20000,
                         take=2)
        buf = bytearray(2000)

        def drain():
            _audioif.rx_read(buf)

        seen, fell = watch(out, 3.0, between=drain)
        check("recording while playing: starved() stays 0", max(seen) == 0,
              "max %d" % max(seen))
        _audioif.rx_close()
    finally:
        out.stop()
        out.deinit()
    print("%d legs failed" % len(FAILS) if FAILS else "all legs passed")
    return not FAILS
