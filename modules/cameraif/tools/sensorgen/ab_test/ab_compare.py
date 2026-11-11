# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Compare ab_capture.py's runs: does the description drive the OV2710 as the C driver does?

    python3 ab_compare.py <folder with driver/, description/ and frozen/> [arm folders...]

For each mode it prints, per arm, the settled brightness and mean colour, how
long exposure took to settle from cold and after each step of the LED, the
colour temperature cameraif estimated, and how far each arm's brightness
histogram is from the driver's. It lists every register whose value differs
from the driver arm's (exposure and gain move with the scene, so those are
expected to differ a little). It writes compare_<mode>.png beside the runs:
the three frames side by side and brightness against time.

What passes: registers equal outside exposure and gain; settled brightness
within 3 of 255 and settling times within a frame or two of the driver's;
histogram distance under 0.05. Anything else is worth a look at the frames.

Needs numpy, pillow and matplotlib.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

ARMS = ("driver", "description", "frozen")
# Exposure and gain: these follow the scene and are left out of the verdict.
AE_REGS = {0x3500, 0x3501, 0x3502, 0x350A, 0x350B}


def settle_ms(rows: list[list], band: float = 0.05) -> int | None:
    """When brightness last entered, and stayed within, `band` of its final value."""
    lumas = [(r[0], r[1]) for r in rows if r[1] is not None]
    if not lumas:
        return None
    final = lumas[-1][1]
    when = lumas[0][0]
    for ms, y in lumas:
        if abs(y - final) > band * max(final, 1):
            when = None
        elif when is None:
            when = ms
    return when


def thumb(path: Path, size: tuple[int, int]) -> np.ndarray:
    """An RGB565 little-endian file as an RGB array."""
    v = np.fromfile(path, dtype="<u2").reshape(size[1], size[0])
    r = ((v >> 11) & 0x1F) * 255 // 31
    g = ((v >> 5) & 0x3F) * 255 // 63
    b = (v & 0x1F) * 255 // 31
    return np.dstack((r, g, b)).astype(np.uint8)


def histogram(rgb: np.ndarray) -> np.ndarray:
    y = rgb @ np.array([0.299, 0.587, 0.114])
    h, _ = np.histogram(y, bins=64, range=(0, 256))
    return h / h.sum()


def compare(root: Path, mode: str) -> None:
    runs = {}
    for arm in ARMS:
        path = root / arm / f"{mode}.json"
        if path.exists():
            runs[arm] = json.loads(path.read_text())
    if "driver" not in runs:
        print(f"{mode}: no driver run, nothing to compare against")
        return
    print(f"\n{mode}")
    print(f"{'arm':12} {'luma':>6} {'mean RGB':>16} {'cold':>6} {'LED on':>7} {'LED off':>8} {'kelvin':>7} {'hist':>6}")
    frames = {}
    base = None
    for arm, run in runs.items():
        jpg = root / arm / f"{mode}.jpg"
        # A run saved without its full-size JPEG is judged from the thumbnail.
        rgb = np.asarray(Image.open(jpg).convert("RGB")) if jpg.exists() else thumb(root / arm / f"{mode}.rgb565", (320, 180))
        frames[arm] = rgb
        h = histogram(thumb(root / arm / f"{mode}.rgb565", (320, 180)))
        if base is None:
            base = h
        mean = rgb.reshape(-1, 3).mean(axis=0)
        luma = run["led_off"][-1][1]
        cells = [settle_ms(run[p]) for p in ("cold", "led_on", "led_off")]
        times = " ".join(f"{'-' if c is None else c:>7}" for c in cells)
        print(
            f"{arm:12} {luma:6.1f} {mean[0]:5.0f} {mean[1]:5.0f} {mean[2]:5.0f} {times} "
            f"{run['color_temp'] or '-':>7} {np.abs(h - base).sum() / 2:6.3f}"
        )
    driver = dict(runs["driver"]["regs"])
    for arm, run in runs.items():
        if arm == "driver":
            continue
        diffs = [(r, driver.get(r), v) for r, v in run["regs"] if driver.get(r) != v]
        moving = [d for d in diffs if d[0] in AE_REGS]
        fixed = [d for d in diffs if d[0] not in AE_REGS]
        print(f"{arm}: {len(fixed)} registers differ from the driver's, {len(moving)} exposure or gain")
        for r, a, b in fixed:
            print(f"    0x{r:04X}: driver {a}, {arm} {b}")

    fig, axes = plt.subplots(2, max(len(frames), 2), figsize=(5 * max(len(frames), 2), 7))
    for ax, (arm, rgb) in zip(axes[0], frames.items()):
        ax.imshow(rgb)
        ax.set_title(arm)
        ax.axis("off")
    timeline = axes[1][0]
    for arm, run in runs.items():
        t0 = 0
        for phase in ("cold", "led_on", "led_off"):
            rows = [r for r in run[phase] if r[1] is not None]
            timeline.plot([t0 + r[0] for r in rows], [r[1] for r in rows], label=f"{arm}" if phase == "cold" else None)
            t0 += rows[-1][0] if rows else 0
    timeline.set_xlabel("ms (cold, LED on, LED off)")
    timeline.set_ylabel("brightness")
    timeline.legend()
    for ax in axes[1][1:]:
        ax.axis("off")
    fig.tight_layout()
    fig.savefig(root / f"compare_{mode}.png", dpi=80)
    print(f"wrote {root / f'compare_{mode}.png'}")


def main() -> None:
    global ARMS
    root = Path(sys.argv[1])
    # Folder names after the root replace the three arms; the first is the baseline.
    if len(sys.argv) > 2:
        ARMS = tuple(sys.argv[2:])
    modes = sorted({p.stem for arm in ARMS for p in (root / arm).glob("*.json")})
    for mode in modes:
        compare(root, mode)


if __name__ == "__main__":
    main()
