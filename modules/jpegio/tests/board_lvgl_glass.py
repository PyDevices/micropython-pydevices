# On a board with a display, LVGL and jpegio: draw a corpus JPEG with
# lv.image and compare what lands in the panel's framebuffer, pixel for pixel,
# with jpegio decoding the same file into a buffer. The framebuffer is what
# the glass shows, so a match means LVGL drew this JPEG on the glass through
# jpegio's decoder.
#
#   mpftp put frames/baseline_jfif_320x240.jpg /baseline_jfif_320x240.jpg
#   mpftp run board_lvgl_glass.py
#
# Either byte order counts as a match (panels differ on RGB565 byte order).
import board_config
import display_driver  # noqa: F401  (LVGL on board_config's display)
import jpegio
import lvgl as lv

PATH = "/baseline_jfif_320x240.jpg"

dec = jpegio.JpegDecoder()
w, h = dec.open(PATH)
ref = bytearray(w * h * 2)
dec.decode(ref)

scr = lv.screen_active()
scr.clean()
scr.set_style_bg_color(lv.color_black(), 0)
img = lv.image(scr)
try:
    import fs_driver  # frozen in by lvgl-micropython's manifest: LVGL reads files through it

    fs_driver.register("S")
    img.set_src("S:" + PATH)
except ImportError:  # no file driver: hand LVGL the bytes
    with open(PATH, "rb") as f:
        data = f.read()
    img.set_src(lv.image_dsc_t({"header": {"w": w, "h": h, "cf": lv.COLOR_FORMAT.RAW}, "data_size": len(data), "data": data}))
img.set_pos(0, 0)
lv.refr_now(None)

d = board_config.display_drv
fb, fb2, nbytes, stride = d.framebuffers()
print("panel %dx%d, image %dx%d, stride %d, second buffer %s" % (d.width, d.height, w, h, stride, fb2 is not None))


def matches(buf):
    same = 0
    for y in range(h):
        row = y * stride
        for x in range(w):
            i, j = row + 2 * x, 2 * (y * w + x)
            a0, a1, b0, b1 = buf[i], buf[i + 1], ref[j], ref[j + 1]
            if (a0 == b0 and a1 == b1) or (a0 == b1 and a1 == b0):
                same += 1
    return same


best = max(matches(fb), matches(fb2) if fb2 is not None else 0)
total = w * h
print("pixels matching jpegio's own decode: %d of %d (%.2f %%)" % (best, total, 100 * best / total))
print("PASS" if best == total else "FAIL")
