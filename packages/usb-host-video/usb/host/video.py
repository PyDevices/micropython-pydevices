# MicroPython USB video host: frames from a webcam on the host port.
# MIT license; Copyright (c) 2026 Brad Barnett
#
#     import usb.core
#     from usb.host.video import Camera
#
#     usb.core.start(classes=("uvc",))
#     cam = Camera(usb.core.find(custom_match=Camera.match), max_size=(320, 240))
#     print(cam.encoding, cam.width, cam.height, cam.fps)    # what it agreed to
#     buf = bytearray(cam.frame_bytes)
#     cam.start()
#     while True:
#         n = cam.read(buf)            # a whole frame (a JPEG, or YUY2), or 0
#
# Any class-compliant camera: a Logitech, or a PyDevices board running
# usb-device-video. The camera's descriptors are read here, a mode is
# negotiated with it (UVC's PROBE and COMMIT: the camera says what a mode
# costs on the bus), and its frames stream in over isochronous transfers,
# moved in C and assembled into whole frames in a ring. MJPEG is preferred
# when the camera offers it, since it carries a far larger picture through
# the same bus.
import _usbif

from machine import USBDevice

from . import _uvc

_SET_CUR = 0x01
_GET_CUR = 0x81
_PROBE = 0x0100             # VS_PROBE_CONTROL << 8
_COMMIT = 0x0200            # VS_COMMIT_CONTROL << 8
# The negotiation block: 26 bytes in UVC 1.0, 34 in 1.1. A 1.0 camera
# answers the first 26, which hold every field read here.
_PROBE_LEN = 34
_CTRL_MS = 3000


def _negotiate(dev, itf, format_index, frame_index, interval):
    """PROBE, read back what the camera will do, COMMIT that verbatim.
    Returns (dwMaxPayloadTransferSize, dwMaxVideoFrameSize): the camera says
    what a mode costs on the bus, and that picks the alternate setting."""
    probe = bytearray(_PROBE_LEN)
    probe[2] = format_index
    probe[3] = frame_index
    probe[4:8] = interval.to_bytes(4, "little")
    dev.ctrl_transfer(0x21, _SET_CUR, _PROBE, itf, probe, _CTRL_MS)
    got = bytearray(_PROBE_LEN)
    dev.ctrl_transfer(0xA1, _GET_CUR, _PROBE, itf, got, _CTRL_MS)
    # Committing anything but what the camera just answered is how a stream
    # ends up in a mode neither side agreed to.
    dev.ctrl_transfer(0x21, _SET_CUR, _COMMIT, itf, got, _CTRL_MS)
    return int.from_bytes(got[22:26], "little"), int.from_bytes(got[18:22], "little")


class Camera:
    @staticmethod
    def match(dev):
        """For usb.core.find(custom_match=...): a device with a video
        streaming interface."""
        return bool(_uvc.formats(dev.configuration_descriptor()))

    def __init__(self, dev, *, encoding=None, max_size=None, fps=None, in_limit=None):
        """Negotiate the largest mode up to ``max_size`` (w, h) the camera
        and the bus can carry. ``encoding`` "mjpeg" or "yuy2" insists on one
        (None prefers MJPEG); ``fps`` asks for a rate (None: the highest that
        fits); ``in_limit`` caps the bytes per isochronous packet, for a host
        controller with a small FIFO (an ESP32-S3's is 600 when it streams
        video)."""
        if dev is None:
            raise ValueError("no camera")
        self.dev = dev
        self._id = dev._id
        blob = dev.configuration_descriptor()
        self._formats = _uvc.formats(blob)
        self._alts = _uvc.alt_settings(blob)
        if not self._formats:
            raise ValueError("not a USB video device")
        self._open = False
        self._ring = None
        self._hdr = bytearray(4)
        self._scratch = None
        self._last = (0,) * 8
        picked = self._negotiate(encoding, max_size, fps, in_limit)
        if picked is None:
            raise ValueError("no mode the camera and this bus can both carry; it offers: "
                             + "; ".join(self.modes()))
        fmt, frame, interval, payload, frame_bytes, alt = picked
        self._fmt, self._alt = fmt, alt
        self.encoding = fmt.encoding
        self.width, self.height = frame.width, frame.height
        self.fps = round(_uvc.fps(interval), 2)
        self.payload = payload
        self.frame_bytes = frame_bytes

    def modes(self):
        """What the camera offers, as strings."""
        return [_uvc.describe(f, fr) for f in self._formats for fr in f.frames]

    def _negotiate(self, encoding, max_size, fps, in_limit):
        max_w, max_h = max_size or (10000, 10000)
        found = []
        for fmt in self._formats:
            if encoding and fmt.encoding.lower() != encoding.lower():
                continue
            for frame in fmt.frames:
                if frame.width <= max_w and frame.height <= max_h:
                    rank = 1 if fmt.encoding == "mjpeg" else 0
                    found.append((frame.width * frame.height, rank, fmt, frame))
        found.sort(key=lambda c: (c[0], c[1]), reverse=True)
        for _, _, fmt, frame in found:
            intervals = list(frame.intervals) or [0]
            if fps:
                intervals.sort(key=lambda i: abs(_uvc.fps(i) - fps) if i else 0)
            else:
                intervals.sort()    # shortest interval, the highest rate, first
            for interval in intervals:
                payload, frame_bytes = _negotiate(self.dev, fmt.interface, fmt.index, frame.index, interval)
                if in_limit and payload > in_limit:
                    continue
                alt = _uvc.alt_for_payload(self._alts, payload)
                if alt is not None:
                    return fmt, frame, interval, payload, frame_bytes, alt
        return None

    def start(self):
        if not self._open:
            a = self._alt
            itf = self._fmt.interface
            self.dev.set_interface_altsetting(itf, a.alt)
            # Room for a whole frame waiting while the next one builds, each
            # led by its length; the ring rounds up to a power of two.
            ring = USBDevice.Ring(2 * (self.frame_bytes + 4))
            try:
                _usbif.host_stream(self._id, a.endpoint, ring, a.max_packet, video=True,
                                   max_frame=self.frame_bytes)
            except BaseException:
                self._park()
                raise
            self._ring = ring
            self._open = True

    def read(self, buf):
        """The newest whole frame into buf, returning its length, or 0 when
        no new frame has arrived. Older frames waiting are skipped, and so is
        a frame larger than buf."""
        if not self._open:
            return 0
        r, hdr, n = self._ring, self._hdr, 0
        while r.any() >= 4:
            r.readinto(hdr, 4)
            size = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | hdr[3] << 24
            if size <= len(buf):
                r.readinto(buf, size)
                n = size
            else:
                self._skip(size)
        return n

    def _skip(self, size):
        if self._scratch is None:
            self._scratch = bytearray(512)
        while size > 0:
            size -= self._ring.readinto(self._scratch, min(size, 512))

    def stats(self):
        """(frames, packets, bytes, dropped_full, dropped_torn, dropped_big,
        errors, empty). After stop(), the stream's last."""
        if not self._open:
            return self._last
        st = _usbif.host_stream_stats(self._id, self._alt.endpoint)
        if not st:
            return self._last
        packets, nbytes, full, _, errors, empty, frames, torn, big = st
        return (frames, packets, nbytes, full, torn, big, errors, empty)

    def stop(self):
        if self._open:
            self._last = self.stats()
            _usbif.host_stream(self._id, self._alt.endpoint, None)
            self._open = False
            self._ring = None
            self._park()

    def _park(self):
        # Back to alternate 0, the zero-bandwidth setting, and let go of it,
        # so the camera stops holding isochronous schedule.
        itf = self._fmt.interface
        try:
            self.dev.set_interface_altsetting(itf, 0)
        except OSError:
            pass
        try:
            self.dev._release(itf)
        except OSError:
            pass

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()
