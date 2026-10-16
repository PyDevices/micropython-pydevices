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
# moved in C. MJPEG is preferred when the camera offers it, since it carries
# a far larger picture through the same bus.
import _usbif

from . import _uvc


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
            if encoding and fmt.encoding != encoding:
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
                payload, frame_bytes = _usbif.host_uvc_negotiate(self._id, fmt.interface, fmt.index,
                                                                 frame.index, interval)
                if in_limit and payload > in_limit:
                    continue
                alt = _uvc.alt_for_payload(self._alts, payload)
                if alt is not None:
                    return fmt, frame, interval, payload, frame_bytes, alt
        return None

    def start(self):
        if not self._open:
            a = self._alt
            _usbif.host_uvc_open(self._id, self._fmt.interface, a.alt, a.endpoint, a.max_packet, self.frame_bytes)
            self._open = True

    def read(self, buf):
        """The newest whole frame into buf, returning its length, or 0 when
        no new frame has arrived."""
        n = _usbif.host_uvc_read_frame(buf)
        return n if n > 0 else 0

    def stats(self):
        return _usbif.host_uvc_stats()

    def stop(self):
        if self._open:
            _usbif.host_uvc_close()
            self._open = False

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()
