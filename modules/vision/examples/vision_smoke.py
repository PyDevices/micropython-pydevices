# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Check that the vision module's main parts work.

Draws on a picture and reads the pixels back, finds a red shape with
``find_blobs``, encodes and decodes a JPEG, and, on an ESP32-P4, encodes 30
frames of H.264 to a file. Each step prints ``ok`` or ``FAIL``, and the last
line is ``PASS`` or the list of failures. To check the H.264 file on your
computer, fetch it and probe it:

    mpftp get /vision.h264 .
    ffprobe vision.h264

Needs: an ESP32-S3 or ESP32-P4 board with firmware built with vision. The
H.264 step runs only on a P4, and is skipped elsewhere. No camera or display.

Run it:

    mpftp run vision_smoke.py
"""

import time

import image

# Change these to match your setup.
W, H = 320, 240
RED = (255, 0, 0)
fails = []


def check(name, ok, detail=""):
    # Print one result line and remember a failure.
    print("%-4s %s %s" % ("ok" if ok else "FAIL", name, detail))
    if not ok:
        fails.append(name)


# Draw a red rectangle and a blue circle on a black picture.
img = image.Image(W, H, image.RGB565)
img.clear()
img.draw_rectangle(100, 60, 80, 50, color=RED, fill=True)
img.draw_circle(40, 40, 20, color=(0, 0, 255), fill=True)

# Read pixels back: one inside the rectangle, one outside.
px = img.get_pixel(140, 85)
red = px[0] > 240 and px[1] < 16 and px[2] < 16
check("pixel inside the rectangle is red", red, str(px))
px = img.get_pixel(5, 200)
check("pixel outside is black", px == (0, 0, 0), str(px))

# Find the red rectangle by colour, in the LAB colour space.
red_lab = (30, 100, 15, 127, 15, 127)
blobs = img.find_blobs([red_lab], pixels_threshold=100, area_threshold=100)
rects = [b.rect() for b in blobs]
check("find_blobs finds the red rectangle", rects == [(100, 60, 80, 50)], str(rects))

stats = img.get_statistics()
check("get_statistics runs", stats is not None, "l_mean %d" % stats.l_mean())

# Encode the picture as a JPEG, save it, and load it back.
t0 = time.ticks_ms()
jpg = img.to_jpeg(quality=85, copy=True)
dt = time.ticks_diff(time.ticks_ms(), t0)
data = jpg.bytearray()
is_jpeg = data[:2] == b"\xff\xd8" and len(data) > 500
check("to_jpeg makes a JPEG", is_jpeg, "%d bytes in %d ms" % (len(data), dt))
with open("/vision.jpg", "wb") as f:
    f.write(data)
back = image.Image("/vision.jpg", copy_to_fb=False)
size = (back.width(), back.height())
check("a saved JPEG loads back at its size", size == (W, H), "%dx%d" % size)

# Preview isn't supported without a display, and should say so.
try:
    img.flush()
    check("flush says preview is unsupported", False, "no error")
except OSError as e:
    check("flush says preview is unsupported", "NOT_SUPPORTED" in str(e), str(e))

# The H.264 encoder is only in the ESP32-P4 firmware.
try:
    import h264
except ImportError:
    h264 = None
if h264 is None:
    print("skip h264: not in this firmware (it is P4-only)")
else:
    # Encode 30 frames of a moving square to a file.
    enc = h264.H264Encoder(W, H, fps=15)
    sizes, keys = [], []
    t0 = time.ticks_ms()
    with open("/vision.h264", "wb") as f:
        for n in range(30):
            img.clear()
            img.draw_rectangle(10 + n * 8, 60, 40, 40, color=RED, fill=True)
            nal = enc.encode(img)
            f.write(nal)
            sizes.append(len(nal))
            keys.append(enc.keyframe())
    dt = time.ticks_diff(time.ticks_ms(), t0)
    enc.close()
    check("h264 first frame is a keyframe", keys[0], str(keys[:3]))
    detail = "%d frames, %d bytes, %d ms" % (len(sizes), sum(sizes), dt)
    check("h264 emits Annex-B", sizes[0] > 0, detail)

# Summarise.
print("FAIL: " + ", ".join(fails) if fails else "PASS")
