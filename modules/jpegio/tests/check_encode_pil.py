"""Pillow, a decoder that is not ours, must read back what we encoded.

Every JPEG test_jpegio_encode.py wrote must open in Pillow at its size and
decode to the source it was made from -- the corpus camera frame as jpegio
decodes it (TJpgDec, identical on every port), its top-left crop, the frame
tiled to 720x720, or its luma plane -- at the thresholds test_jpegio_encode.py holds jpegio's own
decode to: 30 dB, 36 dB for quality 95 at 4:4:4. The planted byte-swap file
must fail that, or this check proves nothing.

(A first version asked Pillow and jpegio to agree with each other within
40 dB. That measured libjpeg's chroma upsampling against TJpgDec's, not the
encoder: Pillow's own JPEG of the same camera frame only agrees at 39.4 dB.)

Run with the toolbox Python and a unix MicroPython that has jpegio:

    python check_encode_pil.py MICROPYTHON DIR [DIR ...]
"""

import math
import os
import re
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CAMERA = os.path.join(HERE, "frames", "c920e_320x240_dri.jpg")

DECODE = """
import jpegio, sys
d = jpegio.JpegDecoder()
w, h = d.open(sys.argv[1])
b = bytearray(w * h * 2)
d.decode(b)
open(sys.argv[2], "wb").write(b)
print(w, h)
"""


def expand(v):
    r, g, b = v >> 11, (v >> 5) & 63, v & 31
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def psnr(a, b):
    se = sum((x - y) ** 2 for x, y in zip(a, b))
    return 99.0 if se == 0 else 10 * math.log10(255 * 255 / (se / len(a)))


def main():
    mp, dirs = sys.argv[1], sys.argv[2:]
    with tempfile.TemporaryDirectory() as tmp:
        script = os.path.join(tmp, "decode.py")
        with open(script, "w") as f:
            f.write(DECODE)
        raw = os.path.join(tmp, "src.raw")
        sw, sh = map(int, subprocess.check_output([mp, script, CAMERA, raw], text=True).split())
        data = open(raw, "rb").read()
    src = [expand(data[2 * i] | (data[2 * i + 1] << 8)) for i in range(sw * sh)]

    def source(w, h, gray):
        # the frame, a top-left crop of it, or (720x720) it tiled
        px = [src[(y % sh) * sw + x % sw] for y in range(h) for x in range(w)]
        return [p[1] for p in px] if gray else [c for p in px for c in p]

    worst = 99.0
    for d in dirs:
        for name in sorted(os.listdir(d)):
            if not name.endswith(".jpg"):
                continue
            img = Image.open(os.path.join(d, name))
            img.load()
            w, h = img.size
            gray = "gray" in name
            got = list(img.convert("L" if gray else "RGB").tobytes())
            p = psnr(source(w, h, gray), got)
            need = 36.0 if "q95_444" in name else 30.0  # hw_fast_* (exact=False) included
            if name == "planted_swap.jpg":
                print("%-28s %s %dx%d  %.2f dB: the planted fault, must be < %.0f" % (name, img.mode, w, h, p, need))
                assert p < need, "the check passed a byte-swapped image"
                continue
            m = re.search(r"_(\d+)x(\d+)\.jpg$", name)
            assert (w, h) == ((int(m[1]), int(m[2])) if m else (sw, sh)), (name, img.size)
            worst = min(worst, p - need)
            print("%-28s %s %dx%d  Pillow vs source %.2f dB (>= %.0f)" % (name, img.mode, w, h, p, need))
            assert p >= need, "%s: %.2f dB < %.0f" % (name, p, need)
    print("Pillow reads every file back as its source; smallest margin %.2f dB" % worst)


main()
