"""jpegio.JpegEncoder (and the P4's hardware decode) -- the media Phase 2 gate.

Thresholds were fixed before the first run, on the corpus's real camera frame
(c920e_320x240_dri.jpg, decoded by TJpgDec to RGB565: the source). The
synthetic pattern's 1-px checker is no fair target for a lossy codec, so the
synthetic frames carry only the exactness checks.

  1. software, quality 80, 4:2:0: encode, decode with jpegio, PSNR >= 30 dB;
     quality 95, 4:4:4: >= 36 dB; and quality 50 scores below quality 95;
  2. a planted fault -- the source byte-swapped, encoded as if it were not --
     falls below 30 dB, so the threshold can fail;
  3. swap=True on a byte-swapped copy, and a padded stride, give the same
     bytes as the plain buffer (exact);
  4. GRAY: an 8-bit luma plane round-trips at >= 30 dB;
  5. odd and tiny sizes (37x29, 17x9, 1x1) encode and decode back to size;
  6. bad arguments raise, naming what is wrong; hardware=True raises OSError
     where the chip has no engine;
  7. where there is an engine (the ESP32-P4): hardware encode passes the same
     30 and 36 dB, honours swap and stride, and handles the odd sizes; with
     exact=False (the engine's own RGB565 read) it still passes 30 dB; hardware decode
     of every baseline frame it accepts is within 35 dB of TJpgDec's decode
     and its means within the corpus's 2.0-level tolerance of Pillow's, and
     what it refuses TJpgDec decodes; a second thread encoding at the same
     time gets the same bytes as the first;
  9. ms per 720x720 frame, software and engine (reported, not gated);
  8. 100 more encodes do not trend gc.mem_free() down;

and every encoded file is written to tests/encoded/ (or --out DIR) for
check_encode_pil.py, which decodes each with Pillow on the host.

    builds/unix/standard/micropython -X heapsize=16M modules/jpegio/tests/test_jpegio_encode.py
"""

import gc
import json
import math
import os
import sys
import time

import jpegio

HERE = __file__.rsplit("/", 1)[0] if "/" in __file__ else "."
FRAMES = HERE + "/frames"
ARGV = getattr(sys, "argv", [])[1:]
OUT = ARGV[ARGV.index("--out") + 1] if "--out" in ARGV else HERE + "/encoded"
CAMERA = "c920e_320x240_dri.jpg"

PSNR_Q80 = 30.0
PSNR_Q95_444 = 36.0
PSNR_GRAY = 30.0
PSNR_HW_DECODE = 35.0
MEAN_TOL = 2.0  # test_jpegio.py's tolerance against Pillow's per-channel means


def means565(buf, n):  # as test_jpegio.py computes them for reference.json
    r = g = b = 0
    for i in range(0, 2 * n, 2):
        lo = buf[i]
        hi = buf[i + 1]
        r += hi & 0xF8
        g += ((hi & 0x07) << 5) | ((lo >> 5) << 2)
        b += (lo << 3) & 0xF8
    return (r / n, g / n, b / n)


def read_file(name):
    with open(FRAMES + "/" + name, "rb") as f:
        return f.read()


def expect_raises(exc, fn, *needles):
    try:
        fn()
    except exc as e:
        msg = str(e)
        for needle in needles:
            assert needle in msg, "expected %r in %s: %s" % (needle, type(e).__name__, msg)
        return e
    except Exception as e:  # noqa: BLE001
        raise AssertionError("expected %s, got %s: %s" % (exc.__name__, type(e).__name__, e))
    raise AssertionError("expected %s, nothing raised" % exc.__name__)


def decode(data, hardware=False):
    dec = jpegio.JpegDecoder(hardware=True) if hardware else jpegio.JpegDecoder()
    w, h = dec.open(data)
    buf = bytearray(w * h * 2)
    dec.decode(buf)
    return w, h, buf


def psnr565(a, b):
    """PSNR in dB of two native RGB565 buffers, channels expanded to 8 bits."""
    assert len(a) == len(b)
    se = 0
    for i in range(0, len(a), 2):
        va = a[i] | (a[i + 1] << 8)
        vb = b[i] | (b[i + 1] << 8)
        if va == vb:
            continue
        ra, rb = va >> 11, vb >> 11
        ga, gb = (va >> 5) & 63, (vb >> 5) & 63
        ba, bb = va & 31, vb & 31
        dr = ((ra << 3) | (ra >> 2)) - ((rb << 3) | (rb >> 2))
        dg = ((ga << 2) | (ga >> 4)) - ((gb << 2) | (gb >> 4))
        db = ((ba << 3) | (ba >> 2)) - ((bb << 3) | (bb >> 2))
        se += dr * dr + dg * dg + db * db
    if se == 0:
        return 99.0
    mse = se / (3 * (len(a) // 2))
    return 10 * math.log10(255 * 255 / mse)


def psnr_gray(plane, rgb):
    """PSNR of an 8-bit plane against the green channel of decoded RGB565."""
    se = 0
    for i in range(len(plane)):
        v = rgb[2 * i] | (rgb[2 * i + 1] << 8)
        g6 = (v >> 5) & 63
        d = plane[i] - ((g6 << 2) | (g6 >> 4))
        se += d * d
    if se == 0:
        return 99.0
    return 10 * math.log10(255 * 255 / (se / len(plane)))


def swapped(buf):
    out = bytearray(len(buf))
    for i in range(0, len(buf), 2):  # MicroPython slices take no step
        out[i] = buf[i + 1]
        out[i + 1] = buf[i]
    return out


def padded(buf, w, h, bpp, pad):
    row = w * bpp
    out = bytearray((row + pad) * h)
    for y in range(h):
        out[y * (row + pad):y * (row + pad) + row] = buf[y * row:(y + 1) * row]
    return out


def save(name, data):
    with open(OUT + "/" + name, "wb") as f:
        f.write(data)


def has_engine():
    try:
        jpegio.JpegEncoder(hardware=True)
        return True
    except OSError:
        return False


def main():
    t0 = time.time()
    try:
        os.mkdir(OUT)
    except OSError:
        pass
    w, h, src = decode(read_file(CAMERA))
    print("source: %s decoded by TJpgDec, %dx%d" % (CAMERA, w, h))

    print("== 1. software round trip on the camera frame")
    sw80 = jpegio.JpegEncoder(80, 420, hardware=False)
    t = time.ticks_ms() if hasattr(time, "ticks_ms") else 0
    q80 = sw80.encode(src, w, h)
    ms = time.ticks_diff(time.ticks_ms(), t) if hasattr(time, "ticks_ms") else 0
    assert not sw80.hardware
    p80 = psnr565(src, decode(q80)[2])
    q95 = jpegio.JpegEncoder(95, 444, hardware=False).encode(src, w, h)
    p95 = psnr565(src, decode(q95)[2])
    q50 = jpegio.JpegEncoder(50, hardware=False).encode(src, w, h)
    p50 = psnr565(src, decode(q50)[2])
    print("q80 4:2:0 %d bytes %.2f dB (>= %.0f), %d ms" % (len(q80), p80, PSNR_Q80, ms))
    print("q95 4:4:4 %d bytes %.2f dB (>= %.0f)" % (len(q95), p95, PSNR_Q95_444))
    print("q50 4:2:0 %d bytes %.2f dB (< q95)" % (len(q50), p50))
    assert p80 >= PSNR_Q80 and p95 >= PSNR_Q95_444 and p50 < p95
    assert len(q50) < len(q80) < len(q95)
    save("sw_q80.jpg", q80)
    save("sw_q95_444.jpg", q95)
    save("sw_q50.jpg", q50)

    print("== 2. planted fault: byte-swapped source, encoded as native")
    bad = sw80.encode(swapped(src), w, h)
    pbad = psnr565(src, decode(bad)[2])
    print("swapped source: %.2f dB (must be < %.0f)" % (pbad, PSNR_Q80))
    assert pbad < PSNR_Q80, "the PSNR check cannot tell a byte-order fault"
    save("planted_swap.jpg", bad)  # check_encode_pil.py must reject this one

    print("== 3. swap= and stride= are exact")
    assert sw80.encode(swapped(src), w, h, swap=True) == q80
    assert sw80.encode(padded(src, w, h, 2, 10), w, h, stride=w * 2 + 10) == q80
    print("swap=True and stride=%d give q80's bytes" % (w * 2 + 10))

    print("== 4. GRAY")
    plane = bytearray(w * h)
    for i in range(w * h):
        v = src[2 * i] | (src[2 * i + 1] << 8)
        g6 = (v >> 5) & 63
        plane[i] = (g6 << 2) | (g6 >> 4)
    g = sw80.encode(plane, w, h, format=jpegio.GRAY)
    pg = psnr_gray(plane, decode(g)[2])
    print("gray q80 %d bytes %.2f dB (>= %.0f)" % (len(g), pg, PSNR_GRAY))
    assert pg >= PSNR_GRAY
    assert sw80.encode(padded(plane, w, h, 1, 3), w, h, format=jpegio.GRAY, stride=w + 3) == g
    save("sw_gray.jpg", g)

    print("== 5. odd and tiny sizes")
    for ow, oh in ((37, 29), (17, 9), (1, 1)):
        crop = bytearray(ow * oh * 2)
        for y in range(oh):
            crop[y * ow * 2:(y + 1) * ow * 2] = src[y * w * 2:y * w * 2 + ow * 2]
        data = sw80.encode(crop, ow, oh)
        dw, dh, back = decode(data)
        assert (dw, dh) == (ow, oh), (dw, dh)
        save("sw_%dx%d.jpg" % (ow, oh), data)
        print("%dx%d: %d bytes, decodes to %dx%d" % (ow, oh, len(data), dw, dh))

    print("== 6. bad arguments")
    expect_raises(ValueError, lambda: jpegio.JpegEncoder(0), "quality")
    expect_raises(ValueError, lambda: jpegio.JpegEncoder(101), "quality")
    expect_raises(ValueError, lambda: jpegio.JpegEncoder(80, 422), "subsampling")
    expect_raises(ValueError, lambda: sw80.encode(src, w, h + 1), "too small")
    expect_raises(ValueError, lambda: sw80.encode(src, 0, h), "width and height")
    expect_raises(ValueError, lambda: sw80.encode(src, w, h, stride=w), "stride")
    expect_raises(ValueError, lambda: sw80.encode(src, w, h, format=5), "format")
    expect_raises(TypeError, lambda: sw80.encode(12, w, h))
    engine = has_engine()
    if not engine:
        expect_raises(OSError, lambda: jpegio.JpegEncoder(hardware=True), "no hardware")
        expect_raises(OSError, lambda: jpegio.JpegDecoder(hardware=True), "no hardware")
        auto = jpegio.JpegEncoder()
        assert auto.encode(src, w, h) == q80 and not auto.hardware
        print("no JPEG engine here: hardware=True raises, the default encodes in software")

    if engine:
        print("== 7. the JPEG engine")
        hw = jpegio.JpegEncoder(80, 420, hardware=True)
        t = time.ticks_ms()
        hq = hw.encode(src, w, h)
        ms = time.ticks_diff(time.ticks_ms(), t)
        assert hw.hardware
        ph = psnr565(src, decode(hq)[2])
        print("hw q80 4:2:0 %d bytes %.2f dB (>= %.0f), %d ms" % (len(hq), ph, PSNR_Q80, ms))
        assert ph >= PSNR_Q80
        save("hw_q80.jpg", hq)
        auto = jpegio.JpegEncoder()
        assert auto.encode(src, w, h) == hq and auto.hardware, "the default should use the engine"
        ps = psnr565(src, decode(hw.encode(swapped(src), w, h, swap=True))[2])
        pp = psnr565(src, decode(hw.encode(padded(src, w, h, 2, 10), w, h, stride=w * 2 + 10))[2])
        pbad = psnr565(src, decode(hw.encode(swapped(src), w, h))[2])
        print("hw swap=True %.2f dB, stride %.2f dB, planted swap fault %.2f dB" % (ps, pp, pbad))
        assert ps >= PSNR_Q80 and pp >= PSNR_Q80 and pbad < PSNR_Q80
        fast = jpegio.JpegEncoder(80, 420, hardware=True, exact=False)
        hf = fast.encode(src, w, h)
        pf = psnr565(src, decode(hf)[2])
        print("hw exact=False q80 4:2:0 %d bytes %.2f dB (>= %.0f; the engine's zero-fill)" % (len(hf), pf, PSNR_Q80))
        assert pf >= PSNR_Q80 and fast.hardware
        assert psnr565(src, decode(fast.encode(swapped(src), w, h, swap=True))[2]) == pf
        save("hw_fast_q80.jpg", hf)
        h444 = jpegio.JpegEncoder(95, 444, hardware=True).encode(src, w, h)
        p444 = psnr565(src, decode(h444)[2])
        print("hw q95 4:4:4 %d bytes %.2f dB (>= %.0f)" % (len(h444), p444, PSNR_Q95_444))
        assert p444 >= PSNR_Q95_444
        save("hw_q95_444.jpg", h444)
        hg = hw.encode(plane, w, h, format=jpegio.GRAY)
        pg = psnr_gray(plane, decode(hg)[2])
        print("hw gray %d bytes %.2f dB (>= %.0f)" % (len(hg), pg, PSNR_GRAY))
        assert pg >= PSNR_GRAY
        save("hw_gray.jpg", hg)
        for ow, oh in ((37, 29), (17, 9), (1, 1)):
            crop = bytearray(ow * oh * 2)
            for y in range(oh):
                crop[y * ow * 2:(y + 1) * ow * 2] = src[y * w * 2:y * w * 2 + ow * 2]
            try:
                data = hw.encode(crop, ow, oh)
            except OSError as e:
                print("hw %dx%d: refused (%s)" % (ow, oh, e))
                continue
            dw, dh, back = decode(data)
            assert (dw, dh) == (ow, oh)
            p = psnr565(crop, back)
            print("hw %dx%d: %d bytes, %.2f dB" % (ow, oh, len(data), p))
            save("hw_%dx%d.jpg" % (ow, oh), data)

        print("-- hardware decode vs TJpgDec and the corpus reference")
        with open(FRAMES + "/reference.json") as f:
            ref = json.load(f)["frames"]
        for name in sorted(os.listdir(FRAMES)):
            if not name.endswith(".jpg"):
                continue
            data = read_file(name)
            try:
                t = time.ticks_us()
                tw, th, tj = decode(data)
                tus = time.ticks_diff(time.ticks_us(), t)
            except Exception:  # noqa: BLE001
                tj = None
            dec = jpegio.JpegDecoder(hardware=True)
            try:
                w2, h2 = dec.open(data)
            except Exception:  # noqa: BLE001
                assert tj is None
                print("%-36s refused at open() by both" % name)
                continue
            hwbuf = bytearray(w2 * h2 * 2)
            t = time.ticks_us()
            dec.decode(hwbuf)
            us = time.ticks_diff(time.ticks_us(), t)
            if not dec.hardware:
                print("%-36s the engine refused it; TJpgDec decoded it" % name)
                assert hwbuf == tj
                assert name != "baseline_jfif_320x240.jpg", "the engine must decode plain baseline"
                continue
            p = psnr565(tj, hwbuf)
            line = "%-36s engine %d us (TJpgDec %d), %.2f dB vs TJpgDec (>= %.0f)" % (name, us, tus, p, PSNR_HW_DECODE)
            assert p >= PSNR_HW_DECODE, line
            if name in ref:
                want = ref[name]["scales"]["0"]["mean_rgb565"]
                got = means565(hwbuf, w2 * h2)
                err = max(abs(g - r) for g, r in zip(got, want))
                line += ", means %.2f from Pillow's (<= %.1f)" % (err, MEAN_TOL)
                assert err <= MEAN_TOL, line
            print(line)

        # Placement: the engine's image lands at (x, y) of a wider buffer.
        data = read_file("baseline_jfif_64x48.jpg")
        _, _, plain = decode(data, hardware=True)
        big = bytearray(100 * 60 * 2)
        dec = jpegio.JpegDecoder(hardware=True)
        dec.open(data)
        dec.decode(big, 0, 5, 7, stride=100)
        assert dec.hardware
        for y in range(48):
            assert big[((y + 7) * 100 + 5) * 2:((y + 7) * 100 + 69) * 2] == plain[y * 128:(y + 1) * 128]
        assert big[:7 * 200] == bytes(7 * 200)
        dec.open(data)
        dec.decode(big, 1)
        assert not dec.hardware
        print("hw decode at (5, 7) stride 100: placed exactly; scale=1 went to TJpgDec")

        print("-- a second user of the engine, on another thread")
        try:
            import _thread
        except ImportError:
            _thread = None
        if _thread is None:
            print("no _thread here: nothing can use the engine concurrently")
        else:
            done = []

            def other():
                outs = [hw.encode(src, w, h) for _ in range(10)]
                done.append(outs)

            _thread.start_new_thread(other, ())
            mine = [hw.encode(src, w, h) for _ in range(10)]
            t = time.ticks_ms()
            while not done and time.ticks_diff(time.ticks_ms(), t) < 10000:
                time.sleep_ms(10)
            assert done, "the other thread never finished"
            bad = sum(1 for o in mine + done[0] if o != hq)
            print("20 encodes from two threads: %d differ from the single-threaded one" % bad)
            assert bad == 0

    print("== 9. ms per 720x720 frame (the gate's M2)")
    big = bytearray(720 * 720 * 2)
    for y in range(720):
        row = src[(y % h) * w * 2:(y % h) * w * 2 + w * 2]
        for x0 in range(0, 720, w):
            n = min(w, 720 - x0) * 2
            big[(y * 720 + x0) * 2:(y * 720 + x0) * 2 + n] = row[:n]
    encoders = [("software", sw80)]
    if engine:
        encoders += [("engine", jpegio.JpegEncoder(80, 420, hardware=True)),
                     ("engine exact=False", jpegio.JpegEncoder(80, 420, hardware=True, exact=False))]
    for label, enc in encoders:
        t = time.ticks_ms()
        data = enc.encode(big, 720, 720)
        ms = time.ticks_diff(time.ticks_ms(), t)
        print("720x720 q80 4:2:0 %s: %d ms, %d bytes" % (label, ms, len(data)))
        save({"software": "sw", "engine": "hw"}.get(label, "hw_fast") + "_720x720.jpg", data)
    big = None

    print("== 8. memory: 20 encodes, then 100 more")
    # After against after, as test_jpegio.py's section 9 does: the first
    # reading after gc.collect() wobbles by a few KB on the S3 (a conservative
    # collector keeps whatever a stale stack word points at), so a leak shows
    # as a trend between two collected readings, not as before-vs-after.
    for _ in range(20):
        sw80.encode(src, 64, 48)
    gc.collect()
    f1 = gc.mem_free()
    for _ in range(100):
        sw80.encode(src, 64, 48)
    gc.collect()
    f2 = gc.mem_free()
    print("mem_free after 20 encodes %d, after 100 more %d" % (f1, f2))
    assert f2 >= f1 - 1024, "mem_free trends down across encodes"

    print("jpegio encode tests passed, engine %s, %.2f s" % ("yes" if engine else "no", time.time() - t0))


main()
