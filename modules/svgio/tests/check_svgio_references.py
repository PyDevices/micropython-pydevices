# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Compare svgio's renders with the reference PNGs resvg made.

Reads what test_svgio.py --out wrote and compares each render with its
reference in references/, the colour in RGB565 and premultiplied by alpha, so
a pixel nobody sees can't fail. Two renderers anti-alias edges differently, so
the bar is on the whole image and on how many pixels are far off:

- the mean difference, over the four channels, is under MEAN_BAR;
- under FAR_BAR of the pixels differ by more than FAR in any channel.

Blended renders are compared with the reference composited over the same
grey. With --sheet PATH it also saves a picture of every case beside its
reference, to look at.

    python check_svgio_references.py OUT_DIR [--sheet sheet.png]
"""

import argparse
import pathlib
import sys

import numpy as np
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from cases import BACKGROUND, BLENDED, CASES  # noqa: E402

MEAN_BAR = 4.0  # out of 255; a gradient alone is 2, a broken one 28 or more
FAR = 64
FAR_BAR = 0.01  # a fraction of the pixels


def from_565(raw, h, w):
    v = np.frombuffer(raw, dtype="<u2").reshape(h, w).astype(np.uint32)
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1).astype(np.float64)


def to_565_8bit(rgb):
    """RGB888 through RGB565 and back, as svgio stores it."""
    rgb = rgb.astype(np.uint32)
    r, g, b = rgb[..., 0] >> 3, rgb[..., 1] >> 2, rgb[..., 2] >> 3
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1).astype(np.float64)


def score(ours, ref):
    diff = np.abs(ours - ref)
    return diff.mean(), (diff.max(axis=-1) > FAR).mean()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--sheet")
    args = ap.parse_args()
    out = pathlib.Path(args.out)
    bg = from_565(BACKGROUND.to_bytes(2, "little"), 1, 1)[0, 0]
    failed = []
    tiles = []
    for name, width in CASES:
        ref_img = Image.open(HERE / "references" / ("%s-%d.png" % (name, width))).convert("RGBA")
        w, h = ref_img.size
        ref = np.asarray(ref_img).astype(np.float64)
        ref_a = ref[..., 3:4] / 255.0
        ref_pm = np.concatenate([to_565_8bit(ref[..., :3]) * ref_a, ref[..., 3:4]], axis=-1)

        stem = out / ("%s-%d" % (name, width))
        rgb = from_565(stem.with_suffix(".rgb565").read_bytes(), h, w)
        alpha = np.frombuffer(stem.with_suffix(".a8").read_bytes(), dtype=np.uint8).reshape(h, w, 1).astype(np.float64)
        ours_pm = np.concatenate([rgb * alpha / 255.0, alpha], axis=-1)
        mean, far = score(ours_pm, ref_pm)
        ok = mean < MEAN_BAR and far < FAR_BAR
        line = "%-14s mean %.2f, far %.2f%%" % (stem.name, mean, far * 100)

        if name in BLENDED:
            blended = from_565(stem.with_suffix(".blend565").read_bytes(), h, w)
            ref_over = to_565_8bit(ref[..., :3] * ref_a + bg * (1 - ref_a) + 0.5)
            mean_b, far_b = score(blended, ref_over)
            ok = ok and mean_b < MEAN_BAR and far_b < FAR_BAR
            line += "; blended mean %.2f, far %.2f%%" % (mean_b, far_b * 100)
        print(("ok    " if ok else "FAIL  ") + line)
        if not ok:
            failed.append(stem.name)
        tiles.append((ours_pm, ref_pm))

    if args.sheet:
        pad = 4
        sheet_w = max(2 * t[0].shape[1] for t in tiles) + 3 * pad
        sheet_h = sum(t[0].shape[0] + pad for t in tiles) + pad
        sheet = Image.new("RGB", (sheet_w, sheet_h), (255, 255, 255))
        y = pad
        for ours_pm, ref_pm in tiles:
            for k, img in enumerate((ours_pm, ref_pm)):
                a = img[..., 3:4] / 255.0
                over_white = img[..., :3] + 255.0 * (1 - a)
                tile = Image.fromarray(np.clip(over_white, 0, 255).astype(np.uint8))
                sheet.paste(tile, (pad + k * (tile.width + pad), y))
            y += ours_pm.shape[0] + pad
        sheet.save(args.sheet)

    if failed:
        print("svgio differs from the references: " + ", ".join(failed))
        sys.exit(1)
    print("svgio matches the references: %d renders" % len(CASES))


if __name__ == "__main__":
    main()
