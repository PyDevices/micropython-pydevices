"""Live camera preview: a hosted USB webcam shown on the board's own display.

The board is the USB *host* here. ``usb.host.video.Camera`` reads a
class-compliant UVC camera's descriptors, negotiates a video mode with it, and
streams frames in over isochronous transfers -- then this script puts them on
the display that ``board_config`` already set up. No PC in the loop at any
point.

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

import usb.core
from usb.host.video import Camera

app = appdev.App(board_config)

# The camera's data endpoint is fed one packet per bus frame, so a mode is
# only reachable if its negotiated payload fits the host's isochronous IN
# limit. That limit is set by the DWC FIFO split the host chooses from the
# classes it is started for (usbif#2): a session asking for "uvc" gets the
# IN-leaning split, 600 bytes. Camera tries modes largest-first and the first
# one the camera and the bus both accept wins.
IN_LIMIT = 600

# The frame rate to ask the camera for; Camera takes the nearest it offers
# that the bus can carry. None asks for the highest that fits.
FPS = 5

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


def find_camera(timeout_ms=10000):
    """The first device with a video streaming interface."""
    deadline = time.ticks_add(time.ticks_ms(), timeout_ms)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        dev = usb.core.find(custom_match=Camera.match)
        if dev is not None:
            return dev
        time.sleep_ms(250)
    return None


def open_camera(dev, max_size, prefer_mjpeg):
    """Negotiate the largest mode the bus can actually carry.

    Whether a mode *exists* is in the descriptors; how much bandwidth it costs
    is not -- only the camera can say, and it says it by answering PROBE.
    Camera asks for real before it accepts or rejects each candidate, and
    raises ValueError, listing what the camera offers, when none fits.

    When ``prefer_mjpeg`` is true, MJPEG modes are tried first and YUY2 is the
    fallback; otherwise only YUY2 is considered.
    """
    last = None
    for encoding in (("mjpeg", "YUY2") if prefer_mjpeg else ("YUY2",)):
        try:
            return Camera(dev, encoding=encoding, max_size=max_size, fps=FPS, in_limit=IN_LIMIT)
        except ValueError as e:
            last = e
    print("no mode fits this host's isochronous IN limit")
    print("  ", last)
    return None


def _scale_geometry(cam):
    scale = min(display_drv.width // cam.width,
                display_drv.height // cam.height) or 1
    if UPSCALE:
        scale = min(scale, UPSCALE)
    out_w = cam.width * scale
    out_h = cam.height * scale
    x0 = (display_drv.width - out_w) // 2
    y0 = (display_drv.height - out_h) // 2
    return scale, out_w, out_h, x0, y0


usb.core.start(classes=("uvc",))
dev = find_camera()
cam = None
if dev is None:
    print("no UVC camera found on the host port")
else:
    print("camera is", dev, dev.manufacturer, dev.product)
    if _HAVE_JPEGIO:
        print("jpegio present -- preferring MJPEG")
    else:
        print("uncompressed YUY2 only (no jpegio, or PREFER_MJPEG is False)")
    cam = open_camera(dev, MAX_SIZE or (display_drv.width, display_drv.height), _HAVE_JPEGIO)

if cam is not None:
    print("streaming %s %dx%d @ %s fps, up to %d bytes a frame"
          % (cam.encoding, cam.width, cam.height, cam.fps, cam.frame_bytes))
    print("payload %d B/frame" % cam.payload)

    scale, out_w, out_h, x0, y0 = _scale_geometry(cam)
    print("%dx%d upscaled x%d -> %dx%d at (%d, %d)"
          % (cam.width, cam.height, scale, out_w, out_h, x0, y0))

    src = bytearray(cam.frame_bytes)
    cam.start()
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
                  % (_shown, _shown * 1000 / dt, cam.stats()))

    if cam.encoding == "mjpeg":
        # The P4's JPEG engine decodes a whole frame several times faster than
        # TJpgDec; other chips refuse hardware=True, and an older jpegio
        # doesn't know the keyword.
        try:
            decoder = jpegio.JpegDecoder(hardware=True)
        except (OSError, TypeError):
            decoder = jpegio.JpegDecoder()
        # Native-order RGB565, tight. Sized for the negotiated frame; a camera
        # that sends a larger JPEG than it advertised is refused by decode.
        rgb = bytearray(cam.width * cam.height * 2)
        # Nearest-neighbour upscale into a band, same reason as the YUY2 path:
        # blit_rect byteswaps in place, so one band / one blit / one swap.
        band_stride = out_w * 2
        band = bytearray(band_stride * scale)
        band_mv = memoryview(band)
        rgb_mv = memoryview(rgb)
        src_stride = cam.width * 2

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
                for sy in range(cam.height):
                    at = (y0 + sy * scale) * row + x0 * 2
                    rgb565_row_upscale(rgb_mv[sy * src_stride:(sy + 1) * src_stride], fmv[at:],
                                       cam.width, scale, stride)
            if TEAR_FREE:
                display_drv.show()
            _count_frame()

        def _tick(_=None):
            n = cam.read(src)
            if n <= 0:
                return
            try:
                decoder.open(memoryview(src)[:n])
            except Exception:
                return
            if decoder.width != cam.width or decoder.height != cam.height:
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
            for sy in range(cam.height):
                rgb565_row_upscale(rgb_mv[sy * src_stride:(sy + 1) * src_stride], band_mv, cam.width, scale, out_w)
                display_drv.blit_rect(band, x0, y0 + sy * scale, out_w, scale)
            _frame_shown()

    else:
        # Uncompressed YUY2 path.
        band_stride = out_w * 2
        band = bytearray(band_stride * scale)
        band_mv = memoryview(band)
        src_mv = memoryview(src)
        src_stride = cam.width * 2
        whole_frame = src_stride * cam.height
        # The whole converted frame, for the one-blit path at scale 1.
        rgb = bytearray(out_w * out_h * 2) if scale == 1 else None

        def _tick(_=None):
            """Blit a frame if one has arrived; cheap when none has.

            Scheduled rather than looped. A ``while True`` here would work and
            would be wrong: it owns the interpreter, so touch, the REPL and
            anything else the app is running never get a turn.
            """
            n = cam.read(src)
            if n <= 0:
                return
            # A short frame means the camera sent less than a whole picture.
            # Showing it would tear; skip and leave the last good frame up.
            if n < whole_frame:
                return
            if scale == 1:
                # No widening, so the frame converts as one long row and
                # goes to the display in one blit.
                yuy2_row_to_rgb565(src, rgb, cam.width * cam.height, 1)
                display_drv.blit_rect(rgb, x0, y0, out_w, out_h)
                _frame_shown()
                return
            for sy in range(cam.height):
                yuy2_row_to_rgb565(src_mv[sy * src_stride:], band,
                                   cam.width, scale)
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
            cam.stop()
