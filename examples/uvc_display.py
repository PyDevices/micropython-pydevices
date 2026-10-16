"""Live camera preview: a hosted USB webcam shown on the board's own display.

The board is the USB *host* here. It enumerates a class-compliant UVC camera,
reads its descriptors, negotiates a video mode with it, and streams frames in
over isochronous transfers -- then puts them on the display that
``board_config`` already set up. No PC in the loop at any point.

Run it on a board with a display and a camera on its host port::

    mpremote run uvc_display.py

**MJPEG when ``jpegio`` is present.** A webcam offers far better resolutions
in MJPEG than uncompressed -- on the bench camera, 640x480 against 176x144 --
so MJPEG is preferred when the firmware has ``jpegio`` (micropython-pydevices' jpegio module). Frames
arrive as whole JPEGs; ``jpegio.JpegDecoder`` sniffs SOI only, which is what
UVC needs (a UVC MJPEG frame is not JFIF-first, so LVGL's ``is_jpg()`` rejects
it). Without ``jpegio`` the example falls back to uncompressed YUY2, converted
here and blitted to ``display_drv``.

**Why YUY2 is upscaled by whole numbers.** Nearest-neighbour at an integer
factor is a few instructions per pixel and needs no line buffer beyond one
row. Anything smoother is a real resampler, which is a different example.

**Pairing.** A PyDevices board presenting as a webcam (``usb_webcam.py`` on
a P4) is a valid camera for this script on an S3, the same way a Logitech is.
A P4 can be the host too, on a port that supplies 5 V: the
ESP32-P4-WIFI6-DEV-KIT's direct Type-A socket streams a webcam this way.
"""

import time

import appdev
import board_config
import micropython
from board_config import display_drv

import usbif.auto
from usbif import uvc

app = appdev.App(board_config)

# The camera's data endpoint is fed one packet per bus frame, so a mode is
# only reachable if its negotiated payload fits the host's isochronous IN
# limit. That limit is set by the DWC FIFO split the host chooses from the
# classes it is started for (usbif#2): a session asking for "uvc" gets the
# IN-leaning split, 600 bytes. Modes are tried
# largest-first and the first one the camera and the bus both accept wins.
IN_LIMIT = 600

# 100 ns units, which is how UVC counts frame intervals throughout.
INTERVALS = (2000000, 1333333, 1000000, 666666, 333333)   # 5, 7.5, 10, 15, 30 fps

# False tries the uncompressed (YUY2) modes even when jpegio is present.
PREFER_MJPEG = True

# The picture: the largest frame to ask the camera for, and how many times to
# enlarge it on the display. None means "what fits the display". A chip that
# decodes JPEG in software (an ESP32-S3) is quicker asking for a small frame.
# Measured on a Waveshare ESP32-S3-Touch-LCD-4.3 hosting a 320x240 MJPEG
# camera, frames a second:
#
#                          TEAR_FREE = True    TEAR_FREE = False
#   UPSCALE = 1, foreground       7.8                 9.9
#   UPSCALE = 2, foreground       3.2                 3.5
#   UPSCALE = 1, BACKGROUND       4.4                 5.0
#   UPSCALE = 2, BACKGROUND       2.6                 2.9
#
# The S3 decodes a 320x240 JPEG in about 90 ms; the panel's refresh wait is
# what TEAR_FREE = False saves, and the timer what the foreground loop saves.
MAX_SIZE = None
UPSCALE = None

# Decode straight into the display's framebuffer when it shares one (an RGB
# panel's DotClockFramebuffer does): no intermediate picture, no blit. With
# TEAR_FREE each frame goes to the back buffer and show() swaps on the panel's
# next refresh; False paints the buffer on screen and skips the wait, which
# is faster and can show a seam where a new frame meets the old.
TEAR_FREE = True

# True runs the frame loop on the App's 10 ms timer, so the REPL stays usable
# while it plays; False runs it in the foreground until Ctrl-C, which on an
# ESP32-S3 shows about half as many frames again.
BACKGROUND = True

try:
    import jpegio
    _HAVE_JPEGIO = PREFER_MJPEG
except ImportError:
    jpegio = None
    _HAVE_JPEGIO = False


@micropython.viper
def yuy2_row_to_rgb565(src: ptr8, dst: ptr16, width: int, scale: int):  # noqa: F821
    """One YUY2 row to RGB565, widened by an integer factor.

    YUY2 packs two pixels into four bytes -- Y0 U Y1 V -- so the two pixels
    share one chroma sample. The conversion is BT.601 in fixed point; the
    shifts are by 8 so the coefficients are the usual ones times 256.
    """
    i = 0       # source pixel
    j = 0       # source byte
    o = 0       # destination pixel
    while i < width:
        y0 = int(src[j])
        u = int(src[j + 1]) - 128
        y1 = int(src[j + 2])
        v = int(src[j + 3]) - 128
        rd = (351 * v) >> 8
        gd = (86 * u + 179 * v) >> 8
        bd = (444 * u) >> 8

        r = y0 + rd
        g = y0 - gd
        b = y0 + bd
        if r < 0:
            r = 0
        if r > 255:
            r = 255
        if g < 0:
            g = 0
        if g > 255:
            g = 255
        if b < 0:
            b = 0
        if b > 255:
            b = 255
        px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        k = 0
        while k < scale:
            dst[o] = px
            o += 1
            k += 1

        r = y1 + rd
        g = y1 - gd
        b = y1 + bd
        if r < 0:
            r = 0
        if r > 255:
            r = 255
        if g < 0:
            g = 0
        if g > 255:
            g = 255
        if b < 0:
            b = 0
        if b > 255:
            b = 255
        px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        k = 0
        while k < scale:
            dst[o] = px
            o += 1
            k += 1

        i += 2
        j += 4


@micropython.viper
def rgb565_row_upscale(src: ptr16, dst: ptr16, width: int, scale: int, stride: int):  # noqa: F821
    """One RGB565 row enlarged `scale` times both ways: `scale` output rows,
    `stride` pixels apart, each source pixel repeated `scale` times. All in
    native code; a memoryview slice copied from another slice of the same
    buffer costs about 40 ms per row on an ESP32-S3."""
    r = 0
    while r < scale:
        o = r * stride
        x = 0
        while x < width:
            p = src[x]
            k = 0
            while k < scale:
                dst[o] = p
                o += 1
                k += 1
            x += 1
        r += 1


@micropython.viper
def rgb565_copy_down(buf: ptr16, width: int, rows: int):  # noqa: F821
    """Copy a band's first row of `width` pixels into the next `rows - 1`."""
    r = 1
    while r < rows:
        o = r * width
        x = 0
        while x < width:
            buf[o + x] = buf[x]
            x += 1
        r += 1


def find_camera(host, timeout_ms=10000):
    """The first device the enumerator classifies as video."""
    deadline = time.ticks_add(time.ticks_ms(), timeout_ms)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        found = host.find("uvc")
        if found:
            return found[0].id
        time.sleep_ms(250)
    return None


def pick_mode(dev_id, formats, alts, max_w, max_h, prefer_mjpeg):
    """Negotiate the largest mode the bus can actually carry.

    Two separate gates, and they have to be asked in this order. Whether a
    mode *exists* is in the descriptors; how much bandwidth it costs is not --
    only the camera can say, and it says it by answering PROBE. So each
    candidate is negotiated for real before it is accepted or rejected.

    When ``prefer_mjpeg`` is true, MJPEG modes are tried first; otherwise
    only uncompressed encodings are considered.
    """
    candidates = []
    for fmt in formats:
        is_mjpeg = fmt.encoding == "mjpeg"
        if prefer_mjpeg:
            if not is_mjpeg and not fmt.frames:
                continue
            if not is_mjpeg:
                # Prefer MJPEG; keep uncompressed as a fallback pass below.
                continue
        else:
            if is_mjpeg or not fmt.frames:
                continue
        if not fmt.frames:
            continue
        for frame in fmt.frames:
            if frame.width > max_w or frame.height > max_h:
                continue
            # MJPEG first when asked: score by size, then prefer mjpeg in the
            # sort key so equal sizes still land on the compressed path.
            rank = 1 if is_mjpeg else 0
            candidates.append((rank, frame.width * frame.height, fmt, frame))
    candidates.sort(key=lambda c: (c[0], c[1]), reverse=True)

    for _, _, fmt, frame in candidates:
        for interval in INTERVALS:
            if frame.intervals and interval not in frame.intervals:
                continue
            payload, frame_bytes = host.uvc_negotiate(
                dev_id, fmt.interface, fmt.index, frame.index, interval)
            if payload > IN_LIMIT:
                continue
            alt = uvc.alt_for_payload(alts, payload)
            if alt is None:
                continue
            return fmt, frame, interval, payload, frame_bytes, alt
    return None


def _offered(formats):
    for fmt in formats:
        print("   offered:", uvc.describe(fmt))


def _scale_geometry(frame):
    scale = min(display_drv.width // frame.width,
                display_drv.height // frame.height) or 1
    if UPSCALE:
        scale = min(scale, UPSCALE)
    out_w = frame.width * scale
    out_h = frame.height * scale
    x0 = (display_drv.width - out_w) // 2
    y0 = (display_drv.height - out_h) // 2
    return scale, out_w, out_h, x0, y0


host = usbif.auto.host(classes=("uvc",)).start()
dev_id = find_camera(host)
picked = None
if dev_id is None:
    print("no UVC camera found on the host port")
else:
    print("camera is device", dev_id)
    if _HAVE_JPEGIO:
        print("jpegio present -- preferring MJPEG")
    else:
        print("uncompressed YUY2 only (no jpegio, or PREFER_MJPEG is False)")
    blob = host.desc(dev_id)
    max_w, max_h = MAX_SIZE or (display_drv.width, display_drv.height)
    formats = uvc.formats(blob)
    alts = uvc.alt_settings(blob)
    if not formats:
        print("camera declares no video formats")
    else:
        if _HAVE_JPEGIO:
            picked = pick_mode(dev_id, formats, alts, max_w, max_h, True)
        if picked is None:
            picked = pick_mode(dev_id, formats, alts, max_w, max_h, False)
        if picked is None:
            print("no mode fits this host's isochronous IN limit")
            _offered(formats)

if picked is not None:
    fmt, frame, interval, payload, frame_bytes, alt = picked
    print("streaming", uvc.describe(fmt, frame, interval))
    print("payload %d B/frame on alt %d" % (payload, alt.alt))

    scale, out_w, out_h, x0, y0 = _scale_geometry(frame)
    print("%dx%d upscaled x%d -> %dx%d at (%d, %d)"
          % (frame.width, frame.height, scale, out_w, out_h, x0, y0))

    src = bytearray(frame_bytes)
    host.uvc_open(dev_id, fmt.interface, alt.alt, alt.endpoint,
                  alt.max_packet, frame_bytes)
    # This loop presents its own frames: the App's refresh timer would also
    # call show(), waiting on the panel and swapping buffers under it.
    _refresh_claim = app.pause_refresh()
    # Nor does it read touch: the App's 10 ms service tick polling the touch
    # controller over I2C would take its share of every frame.
    _polling_claim = app.pause_polling()
    # Black behind the picture, in both of a double-buffered panel's buffers
    # (the direct paint below may not present again).
    for _ in range(2):
        display_drv.fill(0)
        display_drv.show()
    _shown = 0
    _t0 = time.ticks_ms()

    def _frame_shown():
        display_drv.show()
        _count_frame()

    def _count_frame():
        global _shown
        _shown += 1
        if _shown % 25 == 0:
            dt = time.ticks_diff(time.ticks_ms(), _t0)
            print("%d frames, %.1f fps, stats %r"
                  % (_shown, _shown * 1000 / dt, host.uvc_stats()))

    if fmt.encoding == "mjpeg":
        # The P4's JPEG engine decodes a whole frame several times faster than
        # TJpgDec; other chips refuse hardware=True, and an older jpegio
        # doesn't know the keyword.
        try:
            decoder = jpegio.JpegDecoder(hardware=True)
        except (OSError, TypeError):
            decoder = jpegio.JpegDecoder()
        # Native-order RGB565, tight. Sized for the negotiated frame; a camera
        # that sends a larger JPEG than it advertised is refused by decode.
        rgb = bytearray(frame.width * frame.height * 2)
        # Nearest-neighbour upscale into a band, same reason as the YUY2 path:
        # blit_rect byteswaps in place, so one band / one blit / one swap.
        band_stride = out_w * 2
        band = bytearray(band_stride * scale)
        band_mv = memoryview(band)
        rgb_mv = memoryview(rgb)
        src_stride = frame.width * 2

        # Paint the panel's own buffer when it lets us, and the pixels need
        # no byte swap on the way.
        direct = [getattr(display_drv, "share_framebuffer", False)
                  and not getattr(display_drv, "_requires_byteswap", False)
                  and hasattr(display_drv, "framebuffers")]

        def _target():
            back, front, _, stride = display_drv.framebuffers()
            return (back if TEAR_FREE or front is None else front), stride // 2

        def _direct(n):
            fbuf, stride = _target()
            if scale == 1:
                decoder.decode(fbuf, 0, x0, y0, stride=stride)
            else:
                decoder.decode(rgb, scale=0)
                fmv = memoryview(fbuf)
                row = stride * 2
                for sy in range(frame.height):
                    at = (y0 + sy * scale) * row + x0 * 2
                    rgb565_row_upscale(rgb_mv[sy * src_stride:(sy + 1) * src_stride], fmv[at:],
                                       frame.width, scale, stride)
            if TEAR_FREE:
                display_drv.show()
            _count_frame()

        def _tick(_=None):
            n = host.uvc_read_frame(src)
            if n <= 0:
                return
            try:
                decoder.open(memoryview(src)[:n])
            except Exception:
                return
            if decoder.width != frame.width or decoder.height != frame.height:
                return
            if direct[0]:
                try:
                    _direct(n)
                    return
                except (OSError, ValueError, TypeError) as e:
                    # A decoder or buffer that won't take it (hardware decode
                    # wanting its own buffer): the blit path from now on.
                    print("direct paint off:", e)
                    direct[0] = False
                    decoder.open(memoryview(src)[:n])
            try:
                decoder.decode(rgb, scale=0)
            except Exception:
                return
            if scale == 1:
                # The decoded frame is already the picture: one blit. Row by
                # row cost one call per line, about 448 of them per frame.
                display_drv.blit_rect(rgb, x0, y0, out_w, out_h)
                _frame_shown()
                return
            # Integer upscale row by row into the band, then blit: each row
            # widened in native code, then copied down for the other rows.
            for sy in range(frame.height):
                rgb565_row_upscale(rgb_mv[sy * src_stride:(sy + 1) * src_stride], band_mv, frame.width, scale, out_w)
                display_drv.blit_rect(band, x0, y0 + sy * scale, out_w, scale)
            _frame_shown()

    else:
        # Uncompressed YUY2 path.
        band_stride = out_w * 2
        band = bytearray(band_stride * scale)
        band_mv = memoryview(band)
        src_mv = memoryview(src)
        src_stride = frame.width * 2
        whole_frame = src_stride * frame.height
        # The whole converted frame, for the one-blit path at scale 1.
        rgb = bytearray(out_w * out_h * 2) if scale == 1 else None

        def _tick(_=None):
            """Blit a frame if one has arrived; cheap when none has.

            Scheduled rather than looped. A ``while True`` here would work and
            would be wrong: it owns the interpreter, so touch, the REPL and
            anything else the app is running never get a turn.
            """
            n = host.uvc_read_frame(src)
            if n <= 0:
                return
            # A short frame means the camera sent less than a whole picture.
            # Showing it would tear; skip and leave the last good frame up.
            if n < whole_frame:
                return
            if scale == 1:
                # No widening, so the frame converts as one long row and
                # goes to the display in one blit.
                yuy2_row_to_rgb565(src, rgb, frame.width * frame.height, 1)
                display_drv.blit_rect(rgb, x0, y0, out_w, out_h)
                _frame_shown()
                return
            for sy in range(frame.height):
                yuy2_row_to_rgb565(src_mv[sy * src_stride:], band,
                                   frame.width, scale)
                rgb565_copy_down(band, out_w, scale)
                display_drv.blit_rect(band, x0, y0 + sy * scale, out_w, scale)
            _frame_shown()

    # 10 ms. Frames arrive every 200 ms at 5 fps, so nearly every tick returns
    # immediately.
    if BACKGROUND:
        app.every(_tick, period=10)
    else:
        try:
            while True:
                _tick()
        finally:
            host.uvc_close()
