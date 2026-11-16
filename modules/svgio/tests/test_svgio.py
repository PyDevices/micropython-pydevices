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
  and twice from one open(), the second time from the outlines the first one
  kept, so in well under its time (on MicroPython);
- decode(rows=) draws a band, and a picture drawn in bands of any heights is
  byte for byte the picture drawn whole;
- SvgDecoder(memory=) draws the same pixels from its own working memory,
  asking the heap for nothing, and spills to the heap when it's too small;
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

# The first decode() keeps the outlines it flattened, on the heap, so the
# next one at the same size draws from them; without them nothing stays.
if sys.implementation.name == "micropython":
    import gc

    t = svgio.SvgDecoder()
    tw, th = t.open(svg_path("wifi"), width=24)
    tp, ta = bytearray(tw * th * 2), bytearray(tw * th)
    gc.collect()
    before = gc.mem_alloc()
    t.decode(tp, ta)
    gc.collect()
    kept = gc.mem_alloc() - before
    print("wifi-24: outlines kept %d bytes" % kept)
    assert kept > 0, "decode() should keep the outlines it flattened"
    t = tp = ta = None

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

MICROPYTHON = sys.implementation.name in ("micropython", "circuitpython")


def bands(decoder, cuts, w, h, fill=0):
    """The picture drawn in the bands between cuts: (pixels, alpha)."""
    p, a = bytearray(bytes([fill]) * (w * h * 2)), bytearray(bytes([fill]) * (w * h))
    for k in range(len(cuts) - 1):
        decoder.decode(p, a, rows=(cuts[k], cuts[k + 1]))
    return p, a


# Rows: a picture drawn in bands is the picture drawn whole, whatever the
# bands, and a band touches only its own rows. The gauge is taller than one
# strip, so the cuts fall inside strips and on their edges.
g = svgio.SvgDecoder()
gw, gh = g.open(svg_path("gauge"), width=400)
whole_p, whole_a = bytearray(gw * gh * 2), bytearray(gw * gh)
g.decode(whole_p, whole_a)
for cuts in ((0, gh // 4, gh // 2, 3 * gh // 4, gh), (0, 1, 37, 38, 39, 150, 150, gh - 1, gh)):
    band_p, band_a = bands(g, cuts, gw, gh)
    assert band_p == whole_p and band_a == whole_a, "bands %r differ from the whole picture" % (cuts,)
# An icon is mostly empty, and an empty pixel's colour comes from its
# neighbours in the strip, so bands must not change what a strip holds.
ic = svgio.SvgDecoder()
iw, ih = ic.open(svg_path("gear"), width=300)
icon_p, icon_a = bytearray(iw * ih * 2), bytearray(iw * ih)
ic.decode(icon_p, icon_a)
for cuts in ((0, 97, 194, ih), tuple(range(0, ih, 10)) + (ih,)):
    assert bands(ic, cuts, iw, ih) == (icon_p, icon_a), "an icon's bands %r differ from the whole" % (cuts[:4],)
one_p, one_a = bands(g, (50, 90), gw, gh, 0x5A)
for r in range(gh):
    inside = 50 <= r < 90
    row_a = one_a[r * gw : (r + 1) * gw]
    assert (row_a == whole_a[r * gw : (r + 1) * gw]) if inside else (row_a == b"\x5a" * gw), "row %d" % r
    row_p = one_p[r * gw * 2 : (r + 1) * gw * 2]
    assert (row_p == whole_p[r * gw * 2 : (r + 1) * gw * 2]) if inside else (row_p == b"\x5a" * (gw * 2)), "row %d" % r
# Blended in bands, and into a target only as tall as the rows asked for.
blend_whole, blend_bands = bytearray(b"\x1f\x00" * (gw * gh)), bytearray(b"\x1f\x00" * (gw * gh))
g.decode(blend_whole)
for y0 in range(0, gh, 53):
    g.decode(blend_bands, rows=(y0, min(y0 + 53, gh)))
assert blend_bands == blend_whole, "blended bands differ from the whole picture"
top = bytearray(gw * 10 * 2)
g.decode(top, rows=(0, 10))
expect_raises(ValueError, lambda: g.decode(top, rows=(0, 11)), "target")
expect_raises(ValueError, lambda: g.decode(whole_p, rows=(5, 2)), "rows")
expect_raises(ValueError, lambda: g.decode(whole_p, rows=(0, gh + 1)), "rows")
expect_raises(ValueError, lambda: g.decode(whole_p, rows=(1,)), "rows")

# Memory: a decoder with its own draws the same pixels, whole and in bands,
# over any number of opens, and asks the heap for nothing while it does.
ROOM = 512 * 1024
m = svgio.SvgDecoder(memory=ROOM)
assert m.memory == ROOM and svgio.SvgDecoder().memory == 0
for _ in range(3):
    assert m.open(svg_path("gauge"), width=400) == (gw, gh)
    mem_p, mem_a = bytearray(gw * gh * 2), bytearray(gw * gh)
    m.decode(mem_p, mem_a)
    assert mem_p == whole_p and mem_a == whole_a, "a decoder with memory draws other pixels"
    assert bands(m, (0, 77, 200, gh), gw, gh) == (whole_p, whole_a)
if MICROPYTHON:
    import gc

    assert 0 < m.memory_peak <= ROOM, "the gauge should fit in %d bytes, not %d" % (ROOM, m.memory_peak)
    with open(svg_path("gauge"), "rb") as f:
        text = f.read()
    m.open(text, width=400)
    m.decode(mem_p, mem_a)
    gc.collect()
    before = gc.mem_alloc()
    for _ in range(3):
        m.open(text, width=400)
        m.decode(mem_p, mem_a)
        m.decode(mem_p, mem_a, rows=(10, 60))
    m.decode(mem_p, mem_a)
    # Not collected first: a strip or a shape taken from the heap and given
    # back would show here. The few bytes are the tuples open() returned.
    grew = gc.mem_alloc() - before
    print("gauge-400: working memory peak %d of %d bytes; the heap grew %d" % (m.memory_peak, ROOM, grew))
    assert grew < 1024, "a decoder with memory took %d bytes from the heap" % grew
    assert mem_p == whole_p and mem_a == whole_a
    # With the heap full, it still opens and draws: nothing it needs, not
    # even a strip it would give straight back, comes from there.
    hog = []
    for size in (65536, 4096, 256):
        try:
            while True:
                hog.append(bytearray(size))
        except MemoryError:
            pass
    del hog[-8:]  # room for the tuple open() returns
    try:
        m.open(text, width=400)
        m.decode(mem_p, mem_a)
        full_ok = True
    except MemoryError:
        full_ok = False
    hog = None
    gc.collect()
    assert full_ok, "a decoder with memory needed the heap to open or draw"
    assert mem_p == whole_p and mem_a == whole_a
    # Too small for the picture: what doesn't fit comes from the heap, the
    # pixels are the same, and memory_peak says how much it wanted.
    small = svgio.SvgDecoder(memory=8192)
    small.open(text, width=400)
    small_p, small_a = bytearray(gw * gh * 2), bytearray(gw * gh)
    small.decode(small_p, small_a)
    assert small_p == whole_p and small_a == whole_a, "a decoder that spilled draws other pixels"
    m.open(text, width=400)
    m.decode(mem_p, mem_a)
    assert small.memory < small.memory_peak < 2 * m.memory_peak, "memory_peak should count what spilled: %d, where it all fits in %d" % (
        small.memory_peak,
        m.memory_peak,
    )
    # A decoder without memory counts what it held, too.
    assert g.memory_peak > 0
expect_raises(ValueError, lambda: svgio.SvgDecoder(memory=100), "memory")
print("api: ok")

# Every case, timed; written out for the reference check. Each is drawn
# again by the one decoder with memory, which must give the same bytes.
for name, width in CASES:
    t0 = ticks_us()
    pixels, alpha, w, h = render(name, width)
    us = ticks_us() - t0
    print("%s-%d: %dx%d in %d ms" % (name, width, w, h, us // 1000))
    assert m.open(svg_path(name), width=width) == (w, h)
    mem_p, mem_a = bytearray(w * h * 2), bytearray(w * h)
    m.decode(mem_p, mem_a)
    assert mem_p == pixels and mem_a == alpha, "%s-%d differs with working memory" % (name, width)
    if OUT:
        with open("%s/%s-%d.rgb565" % (OUT, name, width), "wb") as f:
            f.write(pixels)
        with open("%s/%s-%d.a8" % (OUT, name, width), "wb") as f:
            f.write(alpha)
        if name in BLENDED:
            with open("%s/%s-%d.blend565" % (OUT, name, width), "wb") as f:
                f.write(render(name, width, BACKGROUND)[0])
print("svgio: all ok")
