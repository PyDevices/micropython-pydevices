# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""svgio's LVGL image decoder, on MicroPython built beside lvgl.

    micropython test_svgio_lvgl.py

Checks, in order:

1. registration: nothing before lv.init(); after it, one svgio decoder at
   the head of LVGL's list however often you register;
2. lv.image shows SVG text from a variable at the SVG's own size, and the
   screen matches svgio's own render blended over the same background, to
   within a step of RGB565 rounding (LVGL blends RGB565A8 its own way);
3. a file whose name ends in .svg draws the same, through fs_driver;
4. text that isn't SVG, and a file that isn't .svg, are not claimed;
5. refreshing 20 times leaves the heap where it was;
6. lv.deinit() drops the decoder and register_lvgl_decoder() brings it back.

On a firmware without lvgl it checks the other half instead:
register_lvgl_decoder() raises RuntimeError naming the reason,
lvgl_decoders() is (), and it prints SKIP.
"""

import sys

try:
    import lvgl as lv
except ImportError:
    import svgio

    try:
        svgio.register_lvgl_decoder()
    except RuntimeError as e:
        assert "built without its LVGL decoder" in str(e), e
        print("no LVGL decoder compiled in: register_lvgl_decoder() -> RuntimeError: %s" % e)
    else:
        raise AssertionError("register_lvgl_decoder() returned without the decoder compiled in")
    assert svgio.lvgl_decoders() == (), svgio.lvgl_decoders()
    print("no LVGL decoder compiled in: lvgl_decoders() == ()")
    print("SKIP: no `lvgl` module in this interpreter")
    sys.exit(0)

import gc

import svgio

HERE = __file__.rsplit("/", 1)[0] if "/" in __file__ else "."
W, H = 160, 120
DECODER = "svgio"
BACKGROUND = 0x8410  # the screen's colour, grey
NAME, X, Y = "gear", 20, 10
TOLERANCE = 9  # 8-bit units: one RGB565 step of red or blue, and a little


def svg_path(name):
    return HERE + "/svg/" + name + ".svg"


def read(name):
    with open(svg_path(name), "rb") as f:
        return f.read()


def rgb(buf, i):
    v = buf[2 * i] | buf[2 * i + 1] << 8
    return ((v >> 8) & 0xF8, (v >> 3) & 0xFC, (v << 3) & 0xF8)


class Frame:
    """The display's flush target, W x H native RGB565."""

    def __init__(self):
        self.buf = bytearray(W * H * 2)
        self.flushes = 0

    def flush_cb(self, disp, area, color_p):
        aw = area.x2 - area.x1 + 1
        ah = area.y2 - area.y1 + 1
        data = color_p.__dereference__(aw * ah * 2)
        for row in range(ah):
            o = ((area.y1 + row) * W + area.x1) * 2
            self.buf[o : o + aw * 2] = data[row * aw * 2 : (row + 1) * aw * 2]
        self.flushes += 1
        disp.flush_ready()


def setup_display(frame):
    disp = lv.display_create(W, H)
    disp.set_color_format(lv.COLOR_FORMAT.RGB565)
    buf = lv.draw_buf_create(W, H, lv.COLOR_FORMAT.RGB565, 0)
    disp.set_draw_buffers(buf, None)
    disp.set_render_mode(lv.DISPLAY_RENDER_MODE.PARTIAL)
    disp.set_flush_cb(frame.flush_cb)
    scr = lv.screen_active()
    scr.set_style_bg_color(lv.color_hex(0x808080), 0)
    scr.set_style_bg_opa(255, 0)
    return disp, buf


def show(disp, frame, src):
    scr = lv.screen_active()
    scr.clean()
    img = None
    if src is not None:
        img = lv.image(scr)
        img.set_src(src)
        img.set_pos(X, Y)
    frame.flushes = 0
    scr.invalidate()
    lv.refr_now(disp)
    assert frame.flushes > 0, "no flush"
    return img


def image_dsc(data):
    return lv.image_dsc_t({"header": {"w": 1, "h": 1, "cf": lv.COLOR_FORMAT.RAW}, "data_size": len(data), "data": data})


def expected():
    """svgio's own render of NAME, blended over BACKGROUND at (X, Y)."""
    buf = bytearray(W * H * 2)
    for i in range(0, len(buf), 2):
        buf[i], buf[i + 1] = BACKGROUND & 0xFF, BACKGROUND >> 8
    d = svgio.SvgDecoder()
    w, h = d.open(svg_path(NAME))
    d.decode(buf, x=X, y=Y, stride=W)
    return buf, w, h


def compare(got, want, what):
    worst = 0
    for i in range(W * H):
        a, b = rgb(got, i), rgb(want, i)
        worst = max(worst, abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))
    assert worst <= TOLERANCE, "%s: a pixel is %d off svgio's own render" % (what, worst)
    print("%s: matches svgio's own render, worst channel %d" % (what, worst))


def main():
    print("== 1. registration")
    assert svgio.lvgl_decoders() == ()
    try:
        svgio.register_lvgl_decoder()
    except RuntimeError as e:
        assert "lv.init() first" in str(e), e
    else:
        raise AssertionError("registered before lv.init()")
    lv.init()
    import svgio as again  # noqa: F401 -- a second import runs the module's __init__

    svgio.register_lvgl_decoder()
    assert svgio.register_lvgl_decoder() is False
    names = svgio.lvgl_decoders()
    assert names.count(DECODER) == 1 and names[0] == DECODER, names
    print("one decoder, first in line: %s" % (names,))

    print("== 2. SVG text in a variable")
    frame = Frame()
    disp, draw_buf = setup_display(frame)
    want, w, h = expected()
    text = read(NAME)
    dsc = image_dsc(text)
    img = show(disp, frame, dsc)
    assert (img.get_src_width(), img.get_src_height()) == (w, h), (img.get_src_width(), img.get_src_height())
    compare(frame.buf, want, "variable source, %dx%d" % (w, h))

    print("== 3. a .svg file")
    try:
        import fs_driver
    except ImportError:
        fs_driver = None
    if fs_driver is None:
        print("fs_driver not frozen in: file source not checked here")
    else:
        fs_driver.register("S")
        img = show(disp, frame, "S:" + svg_path(NAME))
        assert (img.get_src_width(), img.get_src_height()) == (w, h)
        compare(frame.buf, want, "file source")

    print("== 4. not SVG, not claimed")
    junk = image_dsc(b"hello, not an svg at all " * 4)
    img = show(disp, frame, junk)
    assert (img.get_src_width(), img.get_src_height()) == (1, 1), "svgio claimed non-SVG text"
    if fs_driver is not None:
        img = show(disp, frame, "S:" + HERE + "/cases.py")
        assert (img.get_src_width(), img.get_src_height()) == (0, 0)
    print("non-SVG text falls through to LVGL's own decoder; a .py file draws nothing")

    print("== 5. memory over 20 refreshes")
    img = show(disp, frame, dsc)
    for _ in range(3):
        img.invalidate()
        lv.refr_now(disp)
    gc.collect()
    f0 = gc.mem_free()
    for _ in range(20):
        img.invalidate()
        lv.refr_now(disp)
    gc.collect()
    f1 = gc.mem_free()
    print("mem_free before %d, after %d" % (f0, f1))
    assert f1 >= f0 - 4096, "the heap shrinks across refreshes"

    print("== 6. lv.deinit() and lv.init()")
    lv.deinit()
    assert svgio.lvgl_decoders() == ()
    lv.init()
    assert DECODER not in svgio.lvgl_decoders()
    assert svgio.register_lvgl_decoder() is True
    frame = Frame()
    disp, draw_buf = setup_display(frame)
    keep = image_dsc(text)
    show(disp, frame, keep)
    compare(frame.buf, want, "after the cycle")
    lv.deinit()
    print("svgio LVGL tests passed")


main()
