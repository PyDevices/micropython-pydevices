"""ppa -- the media Phase 5 gate. Thresholds fixed before the first run.

On every port (the software path):
  1. srm at the same size -- every rotation, with and without each mirror --
     matches a pure-Python model pixel for pixel; so does a nearest-neighbour
     scale, and a source block;
  2. RGB565 to RGB888, ARGB8888, GRAY8, YUY2, UYVY and YUV420 match the
     Python model of each layout byte for byte, and back again;
  3. bad arguments raise.
On the ESP32-P4 (ppa.HARDWARE), the PPA against the software path:
  4. rotate and mirror at the same size, RGB565: identical;
  5. a scale: PSNR >= 30 dB on a smooth picture (the PPA may interpolate);
  6. RGB888 / ARGB8888 / GRAY8 within +-1 a byte; YUY2 / UYVY / YUV420 within
     +-2 a byte; GRAY8 / YUY2 / UYVY / YUV420's round trip to RGB565 >= 35 dB
     against software's (software samples 4:2:0 chroma as the PPA does:
     fitted on the P4);
  7. fill: exact; blend: within one RGB565 step a channel of the formula;
  8. ms per operation at 1280x720 (M5), PPA and software.

    micropython test_ppa.py
"""

import math
import time

import ppa

W, H = 32, 24


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def expand(v):
    r, g, b = v >> 11, (v >> 5) & 63, v & 31
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def picture(w, h, smooth=False):
    """RGB565: a gradient; unless smooth, with a hard-edged block and noise."""
    buf = bytearray(w * h * 2)
    seed = 1
    for y in range(h):
        for x in range(w):
            r, g, b = x * 255 // max(1, w - 1), y * 255 // max(1, h - 1), (x + y) * 255 // max(1, w + h - 2)
            if not smooth:
                if w // 4 <= x < w // 2 and h // 4 <= y < h // 2:
                    r, g, b = 255, 32, 0
                seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
                r = (r + (seed >> 16) % 31) & 255
            v = rgb565(r, g, b)
            i = 2 * (y * w + x)
            buf[i] = v & 255
            buf[i + 1] = v >> 8
    return buf


def px(buf, w, x, y):
    i = 2 * (y * w + x)
    return buf[i] | (buf[i + 1] << 8)


def model_srm(src, sw, sh, bw, bh, rot, mx, my, sx=0, sy=0, bsw=None, bsh=None):
    """The documented semantics: scale (nearest) the source block, rotate CCW, mirror."""
    bsw = bsw or sw
    bsh = bsh or sh
    turn = rot in (90, 270)
    uw, uh = (bh, bw) if turn else (bw, bh)
    out = bytearray(bw * bh * 2)
    for oy in range(bh):
        for ox in range(bw):
            x = bw - 1 - ox if mx else ox
            y = bh - 1 - oy if my else oy
            if rot == 90:
                ux, uy = uw - 1 - y, x
            elif rot == 180:
                ux, uy = uw - 1 - x, uh - 1 - y
            elif rot == 270:
                ux, uy = y, uh - 1 - x
            else:
                ux, uy = x, y
            v = px(src, sw, sx + ux * bsw // uw, sy + uy * bsh // uh)
            i = 2 * (oy * bw + ox)
            out[i] = v & 255
            out[i + 1] = v >> 8
    return out


def clamp(v):
    return 0 if v < 0 else (255 if v > 255 else v)


def y_of(r, g, b):
    return clamp((77 * r + 150 * g + 29 * b) >> 8)


def uv_of(r, g, b):
    return (clamp(((-43 * r - 85 * g + 128 * b) >> 8) + 128),
            clamp(((128 * r - 107 * g - 21 * b) >> 8) + 128))


def widen(v):
    """RGB565 to 8 bits as the PPA does it, zero-filled (31 -> 248)."""
    return (v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3


def model_convert(src, w, h, fmt):
    rgb = [widen(px(src, w, x, y)) for y in range(h) for x in range(w)]
    if fmt == ppa.RGB888:
        return bytes(c for r, g, b in rgb for c in (b, g, r))
    if fmt == ppa.ARGB8888:
        return bytes(c for r, g, b in rgb for c in (b, g, r, 255))
    if fmt == ppa.GRAY8:
        return bytes(y_of(*p) for p in rgb)
    if fmt in (ppa.YUY2, ppa.UYVY):
        out = bytearray()
        for i in range(0, w * h, 2):
            a, b2 = rgb[i], rgb[i + 1]
            # the PPA's 4:2:2: U from the pair's left pixel, V from its right
            u, v = uv_of(*a)[0], uv_of(*b2)[1]
            y0, y1 = y_of(*a), y_of(*b2)
            out += bytes((y0, u, y1, v) if fmt == ppa.YUY2 else (u, y0, v, y1))
        return bytes(out)
    if fmt == ppa.YUV420:
        out = bytearray(w * h * 3 // 2)
        line = w * 3 // 2
        for y in range(0, h, 2):
            for x in range(0, w, 2):
                a, b2 = rgb[y * w + x], rgb[y * w + x + 1]
                c, d = rgb[(y + 1) * w + x], rgb[(y + 1) * w + x + 1]
                u, v = uv_of(*a)[0], uv_of(*c)[1]       # the PPA's sampling: U top-left, V bottom-left
                o = y * line + (x // 2) * 3
                out[o:o + 3] = bytes((u, y_of(*a), y_of(*b2)))
                out[o + line:o + line + 3] = bytes((v, y_of(*c), y_of(*d)))
        return bytes(out)


def psnr565(a, b):
    se = 0
    n = len(a) // 2
    for i in range(0, len(a), 2):
        p, q = expand(a[i] | (a[i + 1] << 8)), expand(b[i] | (b[i + 1] << 8))
        se += sum((p[k] - q[k]) ** 2 for k in range(3))
    return 99.0 if se == 0 else 10 * math.log10(255 * 255 * 3 * n / se)


def expect_raises(exc, fn, needle=""):
    try:
        fn()
    except exc as e:
        assert needle in str(e), (needle, str(e))
        return
    raise AssertionError("expected %s" % exc.__name__)


def aligned(n):
    """A buffer the PPA can write: its address and its length whole 64-byte
    cache lines (a bytearray's data is only 16-byte aligned on MicroPython)."""
    n = (n + 63) & ~63
    raw = bytearray(n + 128)
    mv = memoryview(raw)
    addr = 0
    try:
        import uctypes

        addr = uctypes.addressof(raw)
    except ImportError:
        return raw
    off = (-addr) % 64
    return mv[off:off + n]


def main():
    t0 = time.time()
    src = picture(W, H)
    print("== 1. srm, software, against the Python model")
    n = 0
    for rot in (0, 90, 180, 270):
        for mx in (False, True):
            for my in (False, True):
                bw, bh = (H, W) if rot in (90, 270) else (W, H)
                dst = bytearray(bw * bh * 2)
                ppa.srm(src, W, H, dst, bw, bh, rotate=rot, mirror_x=mx, mirror_y=my, hardware=False)
                assert dst == model_srm(src, W, H, bw, bh, rot, mx, my), (rot, mx, my)
                n += 1
    for bw, bh in ((16, 12), (48, 30), (7, 5)):
        dst = bytearray(bw * bh * 2)
        ppa.srm(src, W, H, dst, bw, bh, hardware=False)
        assert dst == model_srm(src, W, H, bw, bh, 0, False, False), (bw, bh)
        n += 1
    dst = bytearray(20 * 10 * 2)
    ppa.srm(src, W, H, dst, 20, 10, src_rect=(4, 2, 20, 10), hardware=False)
    assert dst == model_srm(src, W, H, 20, 10, 0, False, False, 4, 2, 20, 10)
    big = bytearray(40 * 30 * 2)
    ppa.srm(src, W, H, big, 40, 30, x=4, y=3, w=W, h=H, hardware=False)
    assert big[:2 * 40 * 3] == bytes(2 * 40 * 3) and px(big, 40, 4, 3) == px(src, W, 0, 0)
    print("%d rotations, mirrors, scales and blocks: identical to the model" % (n + 2))

    print("== 2. colour conversion, software, against the Python model")
    names = {ppa.RGB888: "RGB888", ppa.ARGB8888: "ARGB8888", ppa.GRAY8: "GRAY8",
             ppa.YUY2: "YUY2", ppa.UYVY: "UYVY", ppa.YUV420: "YUV420"}
    for fmt, name in names.items():
        out = bytearray(ppa.size(fmt, W, H))
        ppa.convert(src, W, H, out, ppa.RGB565, fmt, hardware=False)
        assert bytes(out) == model_convert(src, W, H, fmt), name
        back = bytearray(W * H * 2)
        ppa.convert(out, W, H, back, fmt, ppa.RGB565, hardware=False)
        p = psnr565(src, back)
        print("%-9s matches the model; back to RGB565 at %.1f dB" % (name, p))
        if fmt in (ppa.RGB888, ppa.ARGB8888):
            assert back == src, name     # zero-filled bits narrow back exactly
    sw = bytearray(len(src))
    for i in range(0, len(src), 2):
        sw[i], sw[i + 1] = src[i + 1], src[i]
    out = bytearray(W * H * 2)
    ppa.convert(sw, W, H, out, ppa.RGB565, ppa.RGB565, swap=True, hardware=False)
    assert out == src
    print("swap=True reads big-endian RGB565")
    # a scaled RGB565 -> YUY2 / UYVY into a block (a webcam frame) takes a
    # one-pass path; it must equal scaling first, then converting
    for fmt in (ppa.YUY2, ppa.UYVY):
        bw, bh = 18, 10
        one = bytearray(b"\x55" * (24 * 14 * 2))
        ppa.srm(src, W, H, one, 24, 14, dst_fmt=fmt, x=2, y=3, w=bw, h=bh, hardware=False)
        mid = bytearray(bw * bh * 2)
        ppa.srm(src, W, H, mid, bw, bh, hardware=False)
        blk = bytearray(bw * bh * 2)
        ppa.convert(mid, bw, bh, blk, ppa.RGB565, fmt, hardware=False)
        two = bytearray(b"\x55" * (24 * 14 * 2))
        for j in range(bh):
            at = ((3 + j) * 24 + 2) * 2
            two[at:at + bw * 2] = blk[j * bw * 2:(j + 1) * bw * 2]
        assert one == two, fmt
    print("scaled RGB565 to YUY2 / UYVY in one pass equals scale-then-convert")

    print("== 3. bad arguments")
    expect_raises(ValueError, lambda: ppa.srm(src, W, H, bytearray(10), W, H), "destination buffer")
    expect_raises(ValueError, lambda: ppa.srm(src, W, H, bytearray(W * H * 2), W, H, rotate=45), "rotate")
    expect_raises(ValueError, lambda: ppa.convert(src, 31, 1, bytearray(64), ppa.RGB565, ppa.YUY2), "even")
    expect_raises(ValueError, lambda: ppa.srm(src, W, H, bytearray(W * H * 2), W, H, src_rect=(30, 0, 8, 8)), "source block")
    expect_raises(ValueError, lambda: ppa.convert(src, W, H, bytearray(W * H * 2), ppa.RGB565, 9), "format")
    if not ppa.HARDWARE:
        expect_raises(OSError, lambda: ppa.srm(src, W, H, bytearray(W * H * 2), W, H, hardware=True), "no PPA")
        expect_raises(OSError, lambda: ppa.fill(bytearray(W * H * 2), W, H, 0xFF0000), "no PPA")
        print("no PPA here: hardware=True, fill and blend raise; srm and convert ran in software")
        print("ppa tests passed (software), %.1f s" % (time.time() - t0))
        return

    print("== 4. the PPA: rotate and mirror at the same size, against software")
    big_w, big_h = 320, 240
    src = picture(big_w, big_h)
    for rot in (0, 90, 180, 270):
        for mx, my in ((False, False), (True, False), (False, True)):
            bw, bh = (big_h, big_w) if rot in (90, 270) else (big_w, big_h)
            hw = aligned(bw * bh * 2)
            sw = bytearray(bw * bh * 2)
            assert ppa.srm(src, big_w, big_h, hw, bw, bh, rotate=rot, mirror_x=mx, mirror_y=my, hardware=True)
            ppa.srm(src, big_w, big_h, sw, bw, bh, rotate=rot, mirror_x=mx, mirror_y=my, hardware=False)
            same = bytes(hw)[:len(sw)] == bytes(sw)
            print("rotate %3d mirror_x %d mirror_y %d: %s" % (rot, mx, my, "identical" if same else "DIFFERENT, %.1f dB" % psnr565(bytes(hw)[:len(sw)], sw)))
            assert same

    print("== 5. the PPA: a scale, against software")
    smooth = picture(big_w, big_h, smooth=True)
    for bw, bh in ((640, 480), (160, 120), (200, 90)):
        hw = aligned(bw * bh * 2)
        sw = bytearray(bw * bh * 2)
        ppa.srm(smooth, big_w, big_h, hw, bw, bh, hardware=True)
        ppa.srm(smooth, big_w, big_h, sw, bw, bh, hardware=False)
        p = psnr565(bytes(hw)[:bw * bh * 2], sw)
        print("%dx%d -> %dx%d: %.1f dB (>= 30)" % (big_w, big_h, bw, bh, p))
        assert p >= 30
    # a ratio the PPA's sixteenths can't make exactly: software by default, a reason when insisted on
    odd = aligned(213 * 100 * 2)
    assert ppa.srm(smooth, big_w, big_h, odd, 213, 100) is False
    expect_raises(OSError, lambda: ppa.srm(smooth, big_w, big_h, odd, 213, 100, hardware=True), "sixteenths")
    expect_raises(OSError, lambda: ppa.srm(smooth, big_w, big_h, bytearray(160 * 120 * 2 + 16)[16:], 160, 120, hardware=True), "aligned")
    print("213x100 (not a sixteenth): software by default, refused with a reason when hardware=True; an unaligned buffer likewise")

    print("== 6. the PPA: colour conversion, against software")
    for fmt, name in names.items():
        hw = aligned(ppa.size(fmt, big_w, big_h))
        sw = bytearray(len(hw))
        try:
            ppa.convert(smooth, big_w, big_h, hw, ppa.RGB565, fmt, hardware=True)
        except OSError as e:
            # IDF gives the PPA YUV422 and GRAY8 only on revision 3 chips
            assert "revision 3" in str(e), e
            print("%-9s not on this P4's PPA (before revision 3): software only" % name)
            continue
        ppa.convert(smooth, big_w, big_h, sw, ppa.RGB565, fmt, hardware=False)
        hwb = bytes(hw)[:len(sw)]
        worst = max(abs(a - b) for a, b in zip(hwb, sw))
        tol = 2 if fmt in (ppa.YUY2, ppa.UYVY, ppa.YUV420) else 1
        line = "%-9s worst byte %d (<= %d)" % (name, worst, tol)
        if fmt != ppa.RGB888 and fmt != ppa.ARGB8888:
            # and the PPA reads it back (GRAY8 too: grey to RGB565 is exact)
            bh_ = aligned(big_w * big_h * 2)
            bs_ = bytearray(big_w * big_h * 2)
            back = "PPA"
            try:
                ppa.convert(hwb, big_w, big_h, bh_, fmt, ppa.RGB565, hardware=True)
            except OSError as e:
                # before revision 3 the PPA can't read YUV422 back: software decodes it
                assert "revision 3" in str(e), e
                ppa.convert(hwb, big_w, big_h, bh_, fmt, ppa.RGB565, hardware=False)
                back = "CPU"
            ppa.convert(sw, big_w, big_h, bs_, fmt, ppa.RGB565, hardware=False)
            p = psnr565(bytes(bh_)[:len(bs_)], bs_)
            line += ", round trip (decoded on the %s) %.1f dB vs software's (>= 35)" % (back, p)
            assert p >= 35, line
        print(line)
        assert worst <= tol, line

    print("== 7. the PPA: fill and blend")
    hw = aligned(big_w * big_h * 2)
    hw[:] = smooth
    ppa.fill(hw, big_w, big_h, 0xFF8000, x=10, y=20, w=100, h=50)
    want = bytearray(smooth)
    c = rgb565(0xFF, 0x80, 0x00)
    for y in range(20, 70):
        for x in range(10, 110):
            i = 2 * (y * big_w + x)
            want[i], want[i + 1] = c & 255, c >> 8
    assert bytes(hw)[:len(want)] == bytes(want)
    print("fill: identical to the Python reference")
    fg = picture(big_w, big_h)
    out = aligned(big_w * big_h * 2)
    ppa.blend(smooth, fg, out, big_w, big_h, 96)
    worst = 0
    for i in range(0, big_w * big_h * 2, 2 * 97):
        bgp, fgp, op_ = (smooth[i] | smooth[i + 1] << 8), (fg[i] | fg[i + 1] << 8), (out[i] | out[i + 1] << 8)
        for shift, bits in ((11, 31), (5, 63), (0, 31)):
            b_, f_, o_ = (bgp >> shift) & bits, (fgp >> shift) & bits, (op_ >> shift) & bits
            want_c = (f_ * 96 + b_ * (255 - 96)) / 255
            worst = max(worst, abs(o_ - want_c))
    print("blend at alpha 96: worst %.2f steps from the formula (<= 1)" % worst)
    assert worst <= 1

    print("== 8. ms per operation at 1280x720 (M5)")
    hd = bytearray(1280 * 720 * 2)
    out = aligned(1280 * 720 * 2)
    out2 = aligned(720 * 1280 * 2)
    for label, fn in (
        ("scale 320x240 -> 1280x720", lambda hwf: ppa.srm(smooth, big_w, big_h, out, 1280, 720, hardware=hwf)),
        ("rotate 90", lambda hwf: ppa.srm(hd, 1280, 720, out2, 720, 1280, rotate=90, hardware=hwf)),
        ("RGB565 -> YUV420", lambda hwf: ppa.convert(hd, 1280, 720, out, ppa.RGB565, ppa.YUV420, hardware=hwf)),
    ):
        times = []
        for hwf in (True, False):
            t = time.ticks_ms()
            fn(hwf)
            times.append(time.ticks_diff(time.ticks_ms(), t))
        print("%-28s PPA %4d ms, software %5d ms" % (label, times[0], times[1]))
    t = time.ticks_ms()
    ppa.fill(out, 1280, 720, 0x203040)
    print("%-28s PPA %4d ms" % ("fill 1280x720", time.ticks_diff(time.ticks_ms(), t)))
    print("ppa tests passed, PPA and software, %.1f s" % (time.time() - t0))


main()
