# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Find pedestrians in a photo and check the boxes against ESP-DL's.

Runs ESP-DL's pedestrian example through the ``vision`` module: the same photo
(``pedestrian.jpg`` from ESP-DL's ``examples/pedestrian_detect``) and
esp-vision's pedestrian model, with the result checked against the boxes
ESP-DL's README lists for it. esp-vision decodes the JPEG to RGB565 where
ESP-DL's example uses RGB888, so boxes are matched by overlap (IoU of at
least 0.7), not to the pixel. The program prints each detection, then ``PASS``
if all three expected boxes were found.

Needs: an ESP32-S3 or ESP32-P4 board with firmware built with vision, the
photo, and esp-vision's ``pedestrian_detect_pico.espdl`` model from
``models/pedestrian_detect/``, both copied to the board:

    mpftp put pedestrian.jpg /pedestrian.jpg
    mpftp put pedestrian_detect_pico.espdl /pedestrian.espdl

Run it:

    mpftp run espdl_pedestrian_test.py
"""

import time

import espdl
import image

# The boxes ESP-DL's README lists, as [score, x1, y1, x2, y2].
GOLDEN = [
    (0.884, 143, 189, 251, 462),
    (0.884, 282, 195, 370, 461),
    (0.805, 412, 224, 486, 394),
]


def iou(a, b):
    # Overlap of two (x, y, width, height) boxes: shared area over total area.
    ax2, ay2, bx2, by2 = a[0] + a[2], a[1] + a[3], b[0] + b[2], b[1] + b[3]
    iw = max(0, min(ax2, bx2) - max(a[0], b[0]))
    ih = max(0, min(ay2, by2) - max(a[1], b[1]))
    inter = iw * ih
    return inter / (a[2] * a[3] + b[2] * b[3] - inter)


# Load the photo as RGB565, which the detector takes.
img = image.Image("/pedestrian.jpg").to_rgb565(copy=True)  # loads as JPEG
print("image %dx%d" % (img.width(), img.height()))
# Load the model and detect, timing the call.
det = espdl.PedestrianDetect("/pedestrian.espdl")
t0 = time.ticks_ms()
found = det.detect(img)
dt = time.ticks_diff(time.ticks_ms(), t0)
print("%d detections in %d ms" % (len(found), dt))
for x, y, w, h, score, cat in found:
    print("  score %.3f box (%d, %d, %d, %d)" % (score, x, y, w, h))

# Pass if the count matches and every expected box overlaps a detection.
ok = len(found) == len(GOLDEN)
for score, x1, y1, x2, y2 in GOLDEN:
    want = (x1, y1, x2 - x1, y2 - y1)
    best = max((iou(want, d[:4]) for d in found), default=0.0)
    print("  golden (%d, %d, %d, %d) best IoU %.2f" % (want + (best,)))
    ok = ok and best >= 0.7
det.deinit()
print("PASS" if ok else "FAIL")
