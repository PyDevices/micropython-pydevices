# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""examples/uvc_display_helpers.py must skip a damaged MJPEG frame.

A camera that shares a busy bus loses a packet now and then, and the frame
it was part of arrives as a JPEG no decoder accepts; a hardware decoder can
also turn down one frame and take the next. The tick that draws frames used
to read either as "this decoder can't paint the panel directly", so the
first such frame turned direct painting off for the rest of the stream.

These tests drive ``make_tick`` with a fake camera, panel and decoder. A
frame nothing can decode is skipped and the next one is still painted
direct; a frame only the direct path refuses is drawn through the blit path
and the next is painted direct again; a decoder that never paints direct
still falls back to the blit path and stays there.
"""

import builtins
import importlib.util
import pathlib
import sys
import time
import types
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

GOOD = b"good"
DAMAGED = b"damaged"        # no path can decode it
STUBBORN = b"stubborn"      # decodes into a buffer, refused direct


def _load_helpers():
    """Import the example's helpers on CPython, with stand-ins for the
    MicroPython-only names they use."""
    fakes = {
        "micropython": types.ModuleType("micropython"),
        "usb": types.ModuleType("usb"),
        "usb.core": types.ModuleType("usb.core"),
        "usb.host": types.ModuleType("usb.host"),
        "usb.host.video": types.ModuleType("usb.host.video"),
    }
    fakes["micropython"].viper = lambda f: f
    fakes["usb.host.video"].Camera = type("Camera", (), {})
    saved = {name: sys.modules.get(name) for name in fakes}
    sys.modules.update(fakes)
    builtins.ptr8 = builtins.ptr16 = object
    try:
        spec = importlib.util.spec_from_file_location(
            "uvc_display_helpers_under_test", ROOT / "examples" / "uvc_display_helpers.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    finally:
        del builtins.ptr8, builtins.ptr16
        for name, old in saved.items():
            if old is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = old
    # The helpers read MicroPython's clock.
    mod.time = types.SimpleNamespace(
        ticks_ms=lambda: int(time.monotonic() * 1000),
        ticks_diff=lambda a, b: a - b,
        ticks_add=lambda a, b: a + b,
        sleep_ms=lambda ms: None,
    )
    return mod


class _Camera:
    encoding = "mjpeg"
    width = 8
    height = 4
    frame_bytes = 64

    def __init__(self, frames):
        self.frames = list(frames)

    def read(self, buf):
        if not self.frames:
            return 0
        frame = self.frames.pop(0)
        buf[:len(frame)] = frame
        return len(frame)

    def stats(self):
        return ()


class _Panel:
    """A panel that shares its framebuffer, so frames can be painted direct."""

    share_framebuffer = True
    _requires_byteswap = False

    def __init__(self):
        self.fb = bytearray(8 * 4 * 2)
        self.shows = 0
        self.blits = 0

    def framebuffers(self):
        return self.fb, None, None, 8 * 2

    def show(self):
        self.shows += 1

    def blit_rect(self, buf, x, y, w, h):
        self.blits += 1


def _jpegio(direct_ok=True):
    """A jpegio whose decoder refuses a damaged frame the way the real one
    does (ValueError), and, with ``direct_ok`` false, refuses to decode into
    a panel's buffer at all."""

    class JpegDecoder:
        width = _Camera.width
        height = _Camera.height
        direct_decodes = 0
        buffer_decodes = 0

        def __init__(self, hardware=False):
            self._frame = b""

        def open(self, data):
            self._frame = bytes(data)

        def decode(self, target, scale=0, x=0, y=0, stride=None):
            if self._frame == DAMAGED:
                raise ValueError("JDR_FMT1: unsupported or malformed JPEG")
            if stride is not None:
                if not direct_ok or self._frame == STUBBORN:
                    raise OSError("decode needs its own buffer")
                JpegDecoder.direct_decodes += 1
            else:
                JpegDecoder.buffer_decodes += 1

    return types.SimpleNamespace(JpegDecoder=JpegDecoder)


class BadFrameTest(unittest.TestCase):
    def setUp(self):
        self.helpers = _load_helpers()

    def _tick(self, frames, jpegio):
        cam = _Camera(frames)
        panel = _Panel()
        geometry = (1, cam.width, cam.height, 0, 0)
        tick = self.helpers.make_tick(cam, panel, jpegio, geometry, True)
        for _ in frames:
            tick()      # must never raise: it runs on a timer
        return panel

    def test_damaged_frame_is_skipped_and_direct_paint_stays_on(self):
        jpegio = _jpegio()
        panel = self._tick([GOOD, DAMAGED, GOOD, GOOD], jpegio)
        self.assertEqual(jpegio.JpegDecoder.direct_decodes, 3)
        self.assertEqual(jpegio.JpegDecoder.buffer_decodes, 0)
        self.assertEqual(panel.shows, 3)
        self.assertEqual(panel.blits, 0)

    def test_decoder_that_cannot_paint_direct_falls_back(self):
        jpegio = _jpegio(direct_ok=False)
        panel = self._tick([GOOD, GOOD, DAMAGED, GOOD], jpegio)
        self.assertEqual(jpegio.JpegDecoder.direct_decodes, 0)
        self.assertEqual(jpegio.JpegDecoder.buffer_decodes, 3)
        self.assertEqual(panel.blits, 3)
        self.assertEqual(panel.shows, 3)

    def test_frame_refused_direct_is_blitted_and_direct_paint_stays_on(self):
        jpegio = _jpegio()
        panel = self._tick([GOOD, STUBBORN, GOOD], jpegio)
        self.assertEqual(jpegio.JpegDecoder.direct_decodes, 2)
        self.assertEqual(jpegio.JpegDecoder.buffer_decodes, 1)
        self.assertEqual(panel.blits, 1)
        self.assertEqual(panel.shows, 3)

    def test_first_frame_damaged(self):
        jpegio = _jpegio()
        panel = self._tick([DAMAGED, GOOD], jpegio)
        self.assertEqual(jpegio.JpegDecoder.direct_decodes, 1)
        self.assertEqual(panel.shows, 1)


if __name__ == "__main__":
    unittest.main()
