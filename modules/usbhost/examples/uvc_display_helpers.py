# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Find a UVC camera and draw its frames, for ``uvc_display.py``.

``find_camera`` and ``open_camera`` locate a UVC camera and negotiate the
largest mode the host's bus can carry. ``scale_geometry`` centres the picture
on the display at a whole-number enlargement, and ``make_tick`` returns the
function that reads one frame from the camera and draws it.

MJPEG frames go through ``jpegio``. When the display shares a framebuffer
(an RGB panel's ``DotClockFramebuffer``) the JPEG is decoded straight into it.
Uncompressed YUY2 frames are converted to RGB565 row by row in native code.
Both are enlarged by nearest neighbour: at a whole-number factor that is a few
instructions per pixel and needs no buffer beyond one band of rows.

Needs: the same board as ``uvc_display.py``, and this file copied next to it
(``mpftp put uvc_display_helpers.py /uvc_display_helpers.py``).
"""

import time

import micropython

import usb.core
from usb.host.video import Camera


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


def open_camera(dev, max_size, prefer_mjpeg, fps, in_limit):
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
            return Camera(dev, encoding=encoding, max_size=max_size, fps=fps, in_limit=in_limit)
        except ValueError as e:
            last = e
    print("no mode fits this host's isochronous IN limit")
    print("  ", last)
    return None


def scale_geometry(cam, display_drv, upscale=None):
    """The enlargement, picture size and top-left corner that centre the frame."""
    scale = min(display_drv.width // cam.width,
                display_drv.height // cam.height) or 1
    if upscale:
        scale = min(scale, upscale)
    out_w = cam.width * scale
    out_h = cam.height * scale
    x0 = (display_drv.width - out_w) // 2
    y0 = (display_drv.height - out_h) // 2
    return scale, out_w, out_h, x0, y0


def make_tick(cam, display_drv, jpegio, geometry, tear_free):
    """Return the function that draws one camera frame, if one has arrived.

    ``geometry`` is what ``scale_geometry`` returned. ``jpegio`` is the module
    (used when the camera sends MJPEG). Every 25 frames the tick prints the
    frame rate and the camera's statistics.
    """
    scale, out_w, out_h, x0, y0 = geometry
    src = bytearray(cam.frame_bytes)
    shown = [0]
    t0 = time.ticks_ms()

    def count_frame():
        shown[0] += 1
        if shown[0] % 25 == 0:
            dt = time.ticks_diff(time.ticks_ms(), t0)
            print("%d frames, %.1f fps, stats %r"
                  % (shown[0], shown[0] * 1000 / dt, cam.stats()))

    def frame_shown():
        display_drv.show()
        count_frame()

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
            return (back if tear_free or front is None else front), stride // 2

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
            if tear_free:
                display_drv.show()
            count_frame()

        def tick(_=None):
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
                frame_shown()
                return
            # Integer upscale row by row into the band, then blit: each row
            # widened in native code, then copied down for the other rows.
            for sy in range(cam.height):
                rgb565_row_upscale(rgb_mv[sy * src_stride:(sy + 1) * src_stride], band_mv, cam.width, scale, out_w)
                display_drv.blit_rect(band, x0, y0 + sy * scale, out_w, scale)
            frame_shown()

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

        def tick(_=None):
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
                frame_shown()
                return
            for sy in range(cam.height):
                yuy2_row_to_rgb565(src_mv[sy * src_stride:], band,
                                   cam.width, scale)
                rgb565_copy_down(band, out_w, scale)
                display_drv.blit_rect(band, x0, y0 + sy * scale, out_w, scale)
            frame_shown()

    return tick
