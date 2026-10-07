"""pngio against Pillow, on the PC, both ways.

    python check_pngio_pillow.py MICROPYTHON DIR

1. Pillow decodes every PNG test_pngio.py wrote into DIR (run it with
   --out DIR first) to exactly the pixels it was given.
2. pngio, in MICROPYTHON, decodes PNGs Pillow made in every layout pngio
   reads (palette and grey at 1, 2, 4 and 8 bits, grey with alpha, RGB,
   RGBA; Pillow's own filters and dynamic Huffman blocks) to Pillow's own
   pixels truncated to RGB565, and refuses an interlaced one.

A planted fault, the comparison made one pixel off, must fail.
"""

import os
import subprocess
import sys

import numpy as np
from PIL import Image

mp, out = sys.argv[1], sys.argv[2]
fails = 0


def to565(rgb):
    rgb = rgb.astype(np.uint16)
    return ((rgb[..., 0] & 0xF8) << 8) | ((rgb[..., 1] & 0xFC) << 3) | (rgb[..., 2] >> 3)


def say(ok, line):
    global fails
    print(("ok   " if ok else "FAIL ") + line)
    fails += 0 if ok else 1


# 1. ours, read by Pillow
for name in sorted(os.listdir(out)):
    if not name.startswith("rt_") or not name.endswith(".png"):
        continue
    raw = np.fromfile(os.path.join(out, name[:-4] + ".raw"), dtype="<u2")
    im = Image.open(os.path.join(out, name))
    im.load()
    got = to565(np.asarray(im.convert("RGB"))).ravel()
    say(im.format == "PNG" and (got == raw).all(), "Pillow reads %s (%s %s)" % (name, im.mode, im.size))
    if name == "rt_grad_64x48.png":
        say(not (np.roll(got, 1) == raw).all(), "planted fault: one pixel off must differ")

# 2. Pillow's, read by pngio
rng = np.random.default_rng(5)
w, h = 33, 21
rgb = rng.integers(0, 256, (h, w, 3), dtype=np.uint8)
rgb[5:15, 5:25] = (200, 40, 90)                         # flat part, so filters vary
cases = {
    "rgb.png": Image.fromarray(rgb, "RGB"),
    "rgba.png": Image.fromarray(np.dstack([rgb, rng.integers(0, 256, (h, w), dtype=np.uint8)]), "RGBA"),
    "gray.png": Image.fromarray(rgb[..., 1], "L"),
    "gray_alpha.png": Image.fromarray(rgb[..., 1], "L").convert("LA"),
    "palette8.png": Image.fromarray(rgb, "RGB").quantize(200),
}
paths = {}
for name, im in cases.items():
    for level in (0, 6, 9):
        p = os.path.join(out, "pil%d_%s" % (level, name))
        im.save(p, "PNG", compress_level=level)
        paths[p] = im
for bits in (1, 2, 4):
    q = Image.fromarray(rgb, "RGB").quantize(1 << bits)
    p = os.path.join(out, "pil_palette%d.png" % bits)
    q.save(p, "PNG", bits=bits)
    paths[p] = q
    g = Image.fromarray((rgb[..., 0] >> (8 - bits) << (8 - bits)).astype(np.uint8), "L")
    p = os.path.join(out, "pil_gray%d.png" % bits)
    g.save(p, "PNG", bits=bits)
    paths[p] = g
# Pillow can't write Adam7 (its interlace option is ignored), so set the IHDR's
# interlace byte on one of its files and re-sign the chunk: pngio must refuse it
import zlib
inter = os.path.join(out, "pil_interlaced.png")
png = bytearray(open(os.path.join(out, "pil6_rgb.png"), "rb").read())
png[28] = 1                                     # IHDR data starts at 16; interlace is its 13th byte
png[29:33] = zlib.crc32(bytes(png[12:29])).to_bytes(4, "big")
open(inter, "wb").write(png)

script = os.path.join(out, "decode_all.py")
with open(script, "w") as f:
    f.write("""import pngio, sys
d = pngio.PngDecoder()
for p in sys.argv[1:]:
    try:
        w, h = d.open(open(p, "rb").read())
        buf = bytearray(w * h * 2)
        d.decode(buf)
        open(p + ".565", "wb").write(buf)
        print("decoded", p)
    except ValueError as e:
        print("refused", p, e)
""")
run = subprocess.run([mp, script] + list(paths) + [inter], capture_output=True, text=True)
lines = run.stdout.splitlines()
for p, im in paths.items():
    want = to565(np.asarray(im.convert("RGB"))).ravel()
    try:
        got = np.fromfile(p + ".565", dtype="<u2")
    except FileNotFoundError:
        got = None
    ok = got is not None and got.shape == want.shape and (got == want).all()
    say(ok, "pngio reads %s (%s)" % (os.path.basename(p), im.mode))
say(any(l.startswith("refused") and "interlaced" in l for l in lines), "pngio refuses an interlaced PNG")
print("FAILED %d" % fails if fails else "PASS")
sys.exit(1 if fails else 0)
