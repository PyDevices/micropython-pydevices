"""Is there a tone at `hz` in this recording? Asked through `audiometer`.

    from tone_check import tone_check
    r = tone_check(buf, 16000, hz=1000)
    r["ok"], r["band"], r["over_db"]

`buf` is a one-channel signed 16-bit recording (an ``array("h")``, which is
what ``audioi2sin.I2SIn(..., bit_depth=16)`` fills) and `rate` is the rate it
claims to be at. The meter cuts the spectrum into log-spaced bands; the
answer is the loudest band, whether its edges hold `hz`, and how far it stands
above the median band, which is the floor.

It passes when the loudest band holds `hz` and stands `need_db` over the
floor. So it fails on silence (nothing stands out), on a recording at the
wrong rate (the tone lands in another band: a 1 kHz tone recorded at 8 kHz
and read as 16 kHz is at 2 kHz), and on a slot with no microphone in it.

Runs on a board and on a desktop build carrying audiodsp.
"""

import audiometer


def tone_check(buf, rate, hz=1000, bands=48, low_hz=100, high_hz=None,
               need_db=20, skip=0):
    import math

    high_hz = high_hz or rate * 0.45
    # Slide the bands so `hz` sits in the middle of one, not on an edge where
    # a tone a few hertz off would split between two.
    step = math.log(high_hz / low_hz) / bands
    k = int(math.log(hz / low_hz) / step)
    low_hz = hz / math.exp((k + 0.5) * step)
    high_hz = low_hz * math.exp(bands * step)
    meter = audiometer.Meter(bands, low_hz=low_hz, high_hz=high_hz)
    view = memoryview(buf)
    sums = [0] * bands
    reads = 0
    seq = None
    chunk = 256
    for at in range(skip, len(buf) - chunk + 1, chunk):
        meter.feed(view[at:at + chunk], rate, 1)
        s, levels, _peak, _rms = meter.levels()
        if s != seq and len(levels) == bands:
            seq = s
            reads += 1
            for i in range(bands):
                sums[i] += levels[i]
    meter.deinit()
    if reads == 0:
        return {"ok": False, "why": "the meter made no reading", "reads": 0}
    # Levels are half-dB steps above -100 dB; average them per band.
    db = [s / reads / 2 - 100 for s in sums]
    band = max(range(bands), key=lambda i: db[i])
    ordered = sorted(db)
    floor = ordered[bands // 2]
    ratio = (high_hz / low_hz) ** (1 / bands)
    lo = low_hz * ratio ** band
    hi = lo * ratio
    over = db[band] - floor
    holds = lo <= hz < hi
    ok = holds and over >= need_db
    why = "ok"
    if not holds:
        why = "the loudest band is %d-%d Hz, not %d Hz" % (lo, hi, hz)
    elif over < need_db:
        why = "the %d Hz band is only %.1f dB over the floor" % (hz, over)
    return {
        "ok": ok,
        "why": why,
        "band": band,
        "band_hz": (round(lo), round(hi)),
        "peak_db": round(db[band], 1),
        "floor_db": round(floor, 1),
        "over_db": round(over, 1),
        "reads": reads,
    }


def report(r):
    if "band" not in r:
        return "FAIL " + r["why"]
    return "%s peak band %d (%d-%d Hz) at %.1f dBFS, floor %.1f dBFS, " \
        "%.1f dB over the floor, %d readings: %s" % (
            "PASS" if r["ok"] else "FAIL", r["band"], r["band_hz"][0],
            r["band_hz"][1], r["peak_db"], r["floor_db"], r["over_db"],
            r["reads"], r["why"])
