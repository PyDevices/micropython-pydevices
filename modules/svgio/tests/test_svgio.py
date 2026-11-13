# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""svgio: sizes, sources, placement, blending, errors and render times.

    micropython test_svgio.py [--out DIR]

Runs on unix MicroPython, on boards, and on CPython with the desktop svgio.
It checks what one runtime can check alone:

- open() gives the SVG's own size, or the size width=, height= or scale=
  ask for, with the height following the width's aspect ratio;
- a path, bytes and a stream read alike;
- decode() with an alpha buffer, with alpha alone, and blended over what the
  target holds; at (x, y) with a stride, touching nothing outside the image;
  and twice from one open();
- bad arguments and data that isn't SVG raise, naming the problem.

With --out, every render in cases.py is written there, raw, for
check_svgio_references.py to compare with the reference PNGs on the PC.
"""

import sys
import time

import svgio

HERE = __file__.rsplit("/", 1)[0] if "/" in __file__ else "."
sys.path.insert(0, HERE)
from cases import BACKGROUND, BLENDED, CASES  # noqa: E402

OUT = None
_argv = getattr(sys, "argv", [])
if "--out" in _argv:
    OUT = _argv[_argv.index("--out") + 1]


def ticks_us():
    try:
        return time.ticks_us()
    except AttributeError:
        return int(time.perf_counter() * 1000000)


def svg_path(name):
    return HERE + "/svg/" + name + ".svg"


def render(name, width, background=None):
    """(pixels, alpha, w, h); with background, blended over that colour instead."""
    d = svgio.SvgDecoder()
    w, h = d.open(svg_path(name), width=width)
    pixels = bytearray(w * h * 2)
    if background is None:
        alpha = bytearray(w * h)
        d.decode(pixels, alpha)
        return pixels, alpha, w, h
    for i in range(0, len(pixels), 2):
        pixels[i] = background & 0xFF
        pixels[i + 1] = background >> 8
    d.decode(pixels)
    return pixels, None, w, h


def expect_raises(exc, fn, text):
    try:
        fn()
    except exc as e:
        assert text in str(e), (text, str(e))
        return
    raise AssertionError("expected %s naming %r" % (exc.__name__, text))


ICON = b'<svg xmlns="http://www.w3.org/2000/svg" width="24" height="12" viewBox="0 0 24 12"><rect x="2" y="2" width="20" height="8" fill="#ff0000"/></svg>'

# Sizes: the SVG's own, then each way of asking for another.
d = svgio.SvgDecoder()
assert d.open(ICON) == (24, 12)
assert (d.width, d.height) == (24, 12)
assert d.open(ICON, width=48) == (48, 24)
assert d.open(ICON, height=6) == (12, 6)
assert d.open(ICON, scale=0.5) == (12, 6)
assert d.open(svg_path("linear"), width=128) == (128, 64)

# A path, bytes and a stream read alike.
with open(svg_path("circle"), "rb") as f:
    text = f.read()
renders = []
for source in (svg_path("circle"), text, bytearray(text), memoryview(text)):
    d.open(source, width=24)
    a = bytearray(24 * 24)
    d.decode(None, a)
    renders.append(bytes(a))
with open(svg_path("circle"), "rb") as f:
    d.open(f, width=24)
    a = bytearray(24 * 24)
    d.decode(None, a)
    renders.append(bytes(a))
assert all(r == renders[0] for r in renders), "sources differ"
assert renders[0][12 * 24 + 12] == 255 and renders[0][0] == 0

# Colour and alpha: a solid red rectangle, its inside opaque red, its
# corners empty.
w, h = d.open(ICON)
pixels, alpha = bytearray(w * h * 2), bytearray(w * h)
d.decode(pixels, alpha)
i = 5 * w + 10
assert alpha[i] == 255 and pixels[2 * i] | pixels[2 * i + 1] << 8 == 0xF800
assert alpha[0] == 0

# Twice from one open() gives the same pixels.
again_p, again_a = bytearray(w * h * 2), bytearray(w * h)
d.decode(again_p, again_a)
assert again_p == pixels and again_a == alpha

# Placed at (x, y) in a wider buffer: the same pixels, and nothing else
# touched.
SW, SH, X, Y = 40, 20, 5, 3
big_p = bytearray(b"\x5a" * (SW * SH * 2))
big_a = bytearray(b"\x5a" * (SW * SH))
d.decode(big_p, big_a, x=X, y=Y, stride=SW)
for r in range(SH):
    for c in range(SW):
        k = r * SW + c
        if Y <= r < Y + h and X <= c < X + w:
            j = (r - Y) * w + (c - X)
            assert big_a[k] == alpha[j] and big_p[2 * k : 2 * k + 2] == pixels[2 * j : 2 * j + 2], (r, c)
        else:
            assert big_a[k] == 0x5A and big_p[2 * k] == 0x5A and big_p[2 * k + 1] == 0x5A, (r, c)

# Blended: where the image is empty the background stays, where it's opaque
# the image's colour replaces it, and an edge lands between the two.
blend = bytearray(w * h * 2)
for k in range(0, len(blend), 2):
    blend[k], blend[k + 1] = 0x1F, 0x00  # blue
d.decode(blend)
assert blend[0] | blend[1] << 8 == 0x001F
assert blend[2 * i] | blend[2 * i + 1] << 8 == 0xF800
edge = render("circle", 48, BACKGROUND)[0]
values = set(edge[k] | edge[k + 1] << 8 for k in range(0, len(edge), 2))
assert len(values) > 3, "a blended circle should have edge shades"

# Errors name the problem.
fresh = svgio.SvgDecoder()
expect_raises(RuntimeError, lambda: fresh.decode(bytearray(4)), "open()")
expect_raises(ValueError, lambda: d.open(ICON, width=10, height=10), "one of")
expect_raises(ValueError, lambda: d.open(ICON, scale=0), "1..32767")
expect_raises(ValueError, lambda: d.open(b"hello, not an svg"), "not an SVG")
d.open(ICON)
expect_raises(ValueError, lambda: d.decode(bytearray(10)), "target")
expect_raises(ValueError, lambda: d.decode(bytearray(w * h * 2), bytearray(10)), "alpha")
expect_raises(ValueError, lambda: d.decode(None), "give a target")
expect_raises(ValueError, lambda: d.decode(bytearray(4000), stride=w - 1), "fit across")
print("api: ok")

# Every case, timed; written out for the reference check.
for name, width in CASES:
    t0 = ticks_us()
    pixels, alpha, w, h = render(name, width)
    us = ticks_us() - t0
    print("%s-%d: %dx%d in %d ms" % (name, width, w, h, us // 1000))
    if OUT:
        with open("%s/%s-%d.rgb565" % (OUT, name, width), "wb") as f:
            f.write(pixels)
        with open("%s/%s-%d.a8" % (OUT, name, width), "wb") as f:
            f.write(alpha)
        if name in BLENDED:
            with open("%s/%s-%d.blend565" % (OUT, name, width), "wb") as f:
                f.write(render(name, width, BACKGROUND)[0])
print("svgio: all ok")
