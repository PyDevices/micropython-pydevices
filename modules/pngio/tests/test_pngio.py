"""pngio on MicroPython: round trips, sizes and times. Runs on unix and on boards.

    micropython test_pngio.py [--out DIR]

Bars fixed before the first run:

- RGB565 out and back is identical (the encoder widens 5/6 bits by
  replication, the decoder truncates them back);
- GS8 decodes to the grey it was; RGB888 to its RGB565 truncation;
- a stride wider than the image, and decode() at (x, y) into a bigger
  target, place pixels where they belong and touch nothing else;
- a flat 480x270 UI frame encodes to under 2 % of the stored 389 KB;
- bad arguments and bad data raise, naming the problem.

With --out, the encoded files are written there for check_pngio_pillow.py,
which decodes them with Pillow on the PC.
"""

import gc
import sys
import time

import pngio

OUT = None
_argv = getattr(sys, "argv", [])
if "--out" in _argv:
    OUT = _argv[_argv.index("--out") + 1]


def ticks():
    try:
        return time.ticks_ms()
    except AttributeError:
        return int(time.time() * 1000)


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def picture(w, h, kind):
    """kind: 'ui' (flat panels and a bar), 'grad' (smooth), 'noise' (worst case)."""
    buf = bytearray(w * h * 2)
    s = 12345
    for y in range(h):
        for x in range(w):
            if kind == "ui":
                v = rgb565(20, 30, 60) if y > 40 else rgb565(200, 200, 210)
                if 100 < y < 170 and (x // 40) % 3 == 0:
                    v = rgb565(255, 180, 0)
            elif kind == "grad":
                v = rgb565(x * 255 // w, y * 255 // h, (x + y) * 255 // (w + h))
            else:
                s = (s * 1103515245 + 12345) & 0x7FFFFFFF
                v = (s >> 8) & 0xFFFF
            i = 2 * (y * w + x)
            buf[i] = v & 255
            buf[i + 1] = v >> 8
    return buf


def roundtrip(buf, w, h, **kw):
    png = pngio.PngEncoder().encode(buf, w, h, **kw)
    d = pngio.PngDecoder()
    assert d.open(png) == (w, h)
    back = bytearray(w * h * 2)
    d.decode(back)
    return png, back


def expect_raises(exc, fn, text):
    try:
        fn()
    except exc as e:
        assert text in str(e), (text, str(e))
        return
    raise AssertionError("no %s (%s)" % (exc.__name__, text))


def main():
    t0 = ticks()
    print("== 1. RGB565 out and back")
    for w, h, kind in ((480, 270, "ui"), (64, 48, "grad"), (37, 11, "noise"), (1, 1, "grad")):
        buf = picture(w, h, kind)
        png, back = roundtrip(buf, w, h)
        assert back == buf, (w, h, kind)
        print("%dx%d %s: identical, %d bytes (stored would be %d)" % (w, h, kind, len(png), h * (1 + 3 * w) + 57))
        if OUT:
            with open("%s/rt_%s_%dx%d.png" % (OUT, kind, w, h), "wb") as f:
                f.write(png)
            with open("%s/rt_%s_%dx%d.raw" % (OUT, kind, w, h), "wb") as f:
                f.write(buf)

    print("== 2. GS8 and RGB888")
    w, h = 40, 30
    g8 = bytearray((x * 7 + y * 3) & 255 for y in range(h) for x in range(w))
    png = pngio.PngEncoder().encode(g8, w, h, format=pngio.GS8)
    back = bytearray(w * h * 2)
    d = pngio.PngDecoder()
    d.open(png)
    d.decode(back)
    for i, v in enumerate(g8):
        want = rgb565(v, v, v)
        assert back[2 * i] | back[2 * i + 1] << 8 == want, (i, v)
    if OUT:
        with open("%s/gs8_40x30.png" % OUT, "wb") as f:
            f.write(png)
    rgb = bytearray((i * 37) & 255 for i in range(w * h * 3))
    png = pngio.PngEncoder(level=6).encode(rgb, w, h, format=pngio.RGB888)
    d.open(png)
    d.decode(back)
    for i in range(w * h):
        want = rgb565(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2])
        assert back[2 * i] | back[2 * i + 1] << 8 == want, i
    print("GS8 decodes to its grey; RGB888 to its RGB565")

    print("== 3. stride, and decode at (x, y)")
    big_w, big_h = 50, 20
    big = picture(big_w, big_h, "grad")
    png = pngio.PngEncoder().encode(big, 30, 10, stride=big_w)     # the top-left 30x10
    target = bytearray(b"\x55" * (60 * 25 * 2))
    d.open(png)
    d.decode(target, 7, 4, stride=60)
    for y in range(25):
        for x in range(60):
            got = target[2 * (y * 60 + x)] | target[2 * (y * 60 + x) + 1] << 8
            if 7 <= x < 37 and 4 <= y < 14:
                src = 2 * ((y - 4) * big_w + (x - 7))
                assert got == big[src] | big[src + 1] << 8, (x, y)
            else:
                assert got == 0x5555, (x, y)
    print("a 30x10 block from a 50-wide stride lands at (7, 4) of a 60x25 target, nothing else touched")

    print("== 4. a UI frame's size, and the time")
    ui = picture(480, 270, "ui")
    for level in (1, 6):
        enc = None
        gc.collect()        # the last encoder's scratch goes before the next one's
        enc = pngio.PngEncoder(level=level)
        enc.encode(ui, 480, 270)
        t = ticks()
        png = enc.encode(ui, 480, 270)
        ms = ticks() - t
        stored = 270 * (1 + 3 * 480) + 57
        print("level %d: %d bytes, %.2f %% of stored, %d ms to encode" % (level, len(png), 100 * len(png) / stored, ms))
        assert len(png) < stored * 0.02
    d.open(png)
    back = bytearray(480 * 270 * 2)
    t = ticks()
    d.decode(back)
    print("decode %d ms" % (ticks() - t))
    assert back == ui

    print("== 5. bad arguments and bad data")
    enc = pngio.PngEncoder()
    expect_raises(ValueError, lambda: enc.encode(bytearray(10), 10, 10), "needs")
    expect_raises(ValueError, lambda: enc.encode(bytearray(200), 10, 10, format=9), "format")
    expect_raises(ValueError, lambda: pngio.PngEncoder(level=0), "level")
    expect_raises(ValueError, lambda: d.open(b"GIF89a" + bytes(40)), "not a PNG")
    good = pngio.PngEncoder().encode(picture(8, 8, "grad"), 8, 8)
    bad = bytearray(good)
    bad[60] ^= 0xFF                 # inside the image data
    d.open(bad)
    expect_raises(ValueError, lambda: d.decode(bytearray(128)), "PNG")
    d.open(good)
    expect_raises(ValueError, lambda: d.decode(bytearray(100)), "needs")
    expect_raises(RuntimeError, lambda: pngio.PngDecoder().decode(bytearray(128)), "open()")
    print("each raises, naming the problem")
    print("pngio tests passed, %d ms" % (ticks() - t0))


main()
