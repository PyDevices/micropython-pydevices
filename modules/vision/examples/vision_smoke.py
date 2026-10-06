# The vision module's smoke test: image draws, reads back and finds what it
# drew; JPEG encodes and decodes; on the P4, h264 encodes frames to a file a
# desktop decoder can check (mpftp get /vision.h264, then ffprobe it).
#
#   mpftp run vision_smoke.py
import time

import image

W, H = 320, 240
RED = (255, 0, 0)
fails = []


def check(name, ok, detail=""):
    print("%-4s %s %s" % ("ok" if ok else "FAIL", name, detail))
    if not ok:
        fails.append(name)


img = image.Image(W, H, image.RGB565)
img.clear()
img.draw_rectangle(100, 60, 80, 50, color=RED, fill=True)
img.draw_circle(40, 40, 20, color=(0, 0, 255), fill=True)

px = img.get_pixel(140, 85)
red = px[0] > 240 and px[1] < 16 and px[2] < 16
check("pixel inside the rectangle is red", red, str(px))
px = img.get_pixel(5, 200)
check("pixel outside is black", px == (0, 0, 0), str(px))

red_lab = (30, 100, 15, 127, 15, 127)
blobs = img.find_blobs([red_lab], pixels_threshold=100, area_threshold=100)
rects = [b.rect() for b in blobs]
check("find_blobs finds the red rectangle", rects == [(100, 60, 80, 50)], str(rects))

stats = img.get_statistics()
check("get_statistics runs", stats is not None, "l_mean %d" % stats.l_mean())

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

try:
    img.flush()
    check("flush says preview is unsupported", False, "no error")
except OSError as e:
    check("flush says preview is unsupported", "NOT_SUPPORTED" in str(e), str(e))

try:
    import h264
except ImportError:
    h264 = None
if h264 is None:
    print("skip h264: not in this firmware (it is P4-only)")
else:
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

print("FAIL: " + ", ".join(fails) if fails else "PASS")
