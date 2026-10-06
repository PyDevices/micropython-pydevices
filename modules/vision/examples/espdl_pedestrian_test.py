# ESP-DL's pedestrian example, run through the vision module: the same photo
# (esp-dl examples/pedestrian_detect/main/pedestrian.jpg) and esp-vision's
# pedestrian model, checked against the boxes ESP-DL's README lists for it.
#
#   mpftp put pedestrian.jpg /pedestrian.jpg
#   mpftp put pedestrian_detect_pico.espdl /pedestrian.espdl
#     (esp-vision's models/pedestrian_detect/)
#   mpftp run espdl_pedestrian_test.py
#
# esp-vision decodes the JPEG to RGB565 where ESP-DL's example uses RGB888,
# so boxes are matched by overlap (IoU >= 0.7), not to the pixel.
import time

import espdl
import image

# ESP-DL's README: [score, x1, y1, x2, y2]
GOLDEN = [
    (0.884, 143, 189, 251, 462),
    (0.884, 282, 195, 370, 461),
    (0.805, 412, 224, 486, 394),
]


def iou(a, b):
    ax2, ay2, bx2, by2 = a[0] + a[2], a[1] + a[3], b[0] + b[2], b[1] + b[3]
    iw = max(0, min(ax2, bx2) - max(a[0], b[0]))
    ih = max(0, min(ay2, by2) - max(a[1], b[1]))
    inter = iw * ih
    return inter / (a[2] * a[3] + b[2] * b[3] - inter)


img = image.Image("/pedestrian.jpg").to_rgb565(copy=True)  # loads as JPEG
print("image %dx%d" % (img.width(), img.height()))
det = espdl.PedestrianDetect("/pedestrian.espdl")
t0 = time.ticks_ms()
found = det.detect(img)
dt = time.ticks_diff(time.ticks_ms(), t0)
print("%d detections in %d ms" % (len(found), dt))
for x, y, w, h, score, cat in found:
    print("  score %.3f box (%d, %d, %d, %d)" % (score, x, y, w, h))

ok = len(found) == len(GOLDEN)
for score, x1, y1, x2, y2 in GOLDEN:
    want = (x1, y1, x2 - x1, y2 - y1)
    best = max((iou(want, d[:4]) for d in found), default=0.0)
    print("  golden (%d, %d, %d, %d) best IoU %.2f" % (want + (best,)))
    ok = ok and best >= 0.7
det.deinit()
print("PASS" if ok else "FAIL")
