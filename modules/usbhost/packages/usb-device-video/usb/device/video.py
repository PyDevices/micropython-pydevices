# MicroPython USB video module: a UVC webcam the computer watches.
# MIT license; Copyright (c) 2026 Brad Barnett
#
#     import usb.device
#     from usb.device.video import CameraInterface
#
#     cam = CameraInterface(width=160, height=120, fps=10)   # YUY2
#     usb.device.get().init(cam, builtin_driver=True)
#     while True:
#         if cam.streaming and cam.wants_frame():
#             cam.write_frame(frame)          # width * height * 2 bytes of YUY2
#
#     cam = CameraInterface(width=640, height=480, fps=30, format="mjpeg",
#                           max_frame=60_000)  # each frame a JPEG, any size up to that
#
#     cam = CameraInterface(sizes=((640, 480), (320, 240)), format="mjpeg")
#     ...                                  # the host picks one; cam.width and
#     ...                                  # cam.height say which, per session
#
# The computer sees a camera (UVC 1.1, one format, one rate, one size or a
# list of them):
# uncompressed YUY2, or MJPEG, where each frame is a JPEG and so far smaller,
# which is how a webcam gets a large picture through a full-speed port. When
# the computer starts watching (selects the streaming alternate setting),
# machine.USBDevice's streaming endpoint sends frames from cam.ring in C,
# from the USB interrupt, each isochronous packet with UVC's payload header.
# Write whole frames: a frame goes out only once all of it is in the ring.
import struct

import machine
from micropython import const

from .core import Interface, split_bmRequestType

_EP_IN_FLAG = const(0x80)
_REQ_TYPE_STANDARD = const(0x00)
_REQ_TYPE_CLASS = const(0x01)
_SET_INTERFACE = const(0x0B)
_GET_INTERFACE = const(0x0A)
_STAGE_SETUP = const(1)
_STAGE_ACK = const(3)

_SET_CUR = const(0x01)
_GET_CUR = const(0x81)
_GET_MIN = const(0x82)
_GET_MAX = const(0x83)
_GET_RES = const(0x84)
_GET_LEN = const(0x85)
_GET_INFO = const(0x86)
_GET_DEF = const(0x87)

_VS_PROBE = const(0x01)
_VS_COMMIT = const(0x02)

_CAMERA_ID = const(1)
_OUTPUT_ID = const(2)

_YUY2 = b"YUY2\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"
_VARIABLE = const(-1)  # USBDevice.stream(): frames led by their length
_PROBE_LEN = const(34)  # UVC 1.1


class CameraInterface(Interface):
    """A webcam: YUY2 or MJPEG, one rate, and one size or several for the
    host to choose from (``sizes``, largest first). The size the host chose
    is ``width`` x ``height`` while it streams; write frames of that size."""

    def __init__(self, *, width=160, height=120, fps=10, format="yuy2", max_frame=None, packet=512,
                 frames_buffered=2, sizes=None, interface_str=None):
        super().__init__()
        if format not in ("yuy2", "mjpeg"):
            raise ValueError("format is 'yuy2' or 'mjpeg'")
        self.sizes = tuple(tuple(sz) for sz in (sizes or ((width, height),)))
        self.fps = fps
        self.format = format
        self.packet = packet
        self.mjpeg = format == "mjpeg"
        # YUY2 frames are all one size; a JPEG is as big as the picture makes
        # it, up to max_frame for the largest size (a quarter of its raw size
        # by default), and in proportion for the others.
        big = self.sizes[0][0] * self.sizes[0][1]
        cap = max_frame or big // 2
        self._frame_bytes = tuple(
            (max(4096, cap * w * h // big) if self.mjpeg else w * h * 2) for w, h in self.sizes)
        slot = max(self._frame_bytes) + (4 if self.mjpeg else 0)
        self.ring = machine.USBDevice.Ring(slot * frames_buffered)
        self.interface_str = interface_str
        self.alt = 0
        self.ep_in = None
        self._interval = 10_000_000 // fps  # 100 ns units
        self._probe_index = 1   # the size the host is probing, 1-based
        self._choose(1)
        self._pending = None
        self._usbd = machine.USBDevice()

    def _choose(self, index):
        """Make size `index` (1-based) the current one."""
        self._index = index
        self.width, self.height = self.sizes[index - 1]
        self.frame_bytes = self._frame_bytes[index - 1]

    def _probe_values(self, index):
        return struct.pack(
            "<HBBIHHHHHIIIBBBB",
            1,  # bmHint: frame interval fixed
            1, index, self._interval,
            0, 0, 0, 0, 0,
            self._frame_bytes[index - 1],
            self.packet,
            48_000_000,
            0, 0, 0, 0,
        )

    # -- descriptors

    def num_itfs(self):
        return 2

    def num_eps(self):
        return 1

    def desc_cfg(self, desc, itf_num, ep_num, strs):
        if self.interface_str:
            strs.append(self.interface_str)
            istr = len(strs) - 1
        else:
            istr = 0
        self.ep_in = ep_num | _EP_IN_FLAG
        self._itf = itf_num
        desc.extend(bytes((8, 0x0B, itf_num, 2, 0x0E, 0x03, 0x00, istr)))  # IAD: video collection
        desc.extend(bytes((9, 0x04, itf_num, 0, 0, 0x0E, 0x01, 0x00, istr)))  # VideoControl
        vc_total = 13 + 18 + 9
        desc.extend(struct.pack("<BBBHHIBB", 13, 0x24, 0x01, 0x0110, vc_total, 48_000_000, 1, itf_num + 1))
        desc.extend(struct.pack("<BBBBHBBHHHB", 18, 0x24, 0x02, _CAMERA_ID, 0x0201, 0, 0, 0, 0, 0, 3)
                    + b"\x00\x00\x00")  # camera terminal, no controls
        desc.extend(struct.pack("<BBBBHBBB", 9, 0x24, 0x03, _OUTPUT_ID, 0x0101, 0, _CAMERA_ID, 0))  # to USB
        desc.extend(bytes((9, 0x04, itf_num + 1, 0, 0, 0x0E, 0x02, 0x00, 0)))  # VideoStreaming alt 0
        n = len(self.sizes)
        vs_total = 14 + (11 if self.mjpeg else 27) + 30 * n + 6
        desc.extend(struct.pack("<BBBBHBBBBBBBB", 14, 0x24, 0x01, 1, vs_total, self.ep_in, 0, _OUTPUT_ID,
                                0, 0, 0, 1, 0))  # input header
        if self.mjpeg:
            desc.extend(bytes((11, 0x24, 0x06, 1, n, 0, 1, 0, 0, 0, 0)))  # format: MJPEG
        else:
            desc.extend(struct.pack("<BBBBB", 27, 0x24, 0x04, 1, n) + _YUY2 + bytes((16, 1, 0, 0, 0, 0)))
        for i, (w, h) in enumerate(self.sizes):
            fb = self._frame_bytes[i]
            rate = fb * 8 * self.fps
            desc.extend(struct.pack("<BBBBBHHIIIIBI", 30, 0x24, 0x07 if self.mjpeg else 0x05, i + 1, 0, w, h,
                                    rate, rate, fb, self._interval, 1, self._interval))  # frame
        desc.extend(bytes((6, 0x24, 0x0D, 1, 1, 4)))  # colour matching: BT.709, sRGB
        desc.extend(bytes((9, 0x04, itf_num + 1, 1, 1, 0x0E, 0x02, 0x00, 0)))  # VideoStreaming alt 1
        desc.extend(struct.pack("<BBBBHB", 7, 0x05, self.ep_in, 0x05, self.packet, 1))  # isochronous IN
        # Class requests to the camera and output terminals name the entity in
        # wIndex's high byte; usb-device's core looks the whole wIndex up.
        from . import get

        itfs = get()._itfs
        for entity in (_CAMERA_ID, _OUTPUT_ID):
            itfs[(entity << 8) | itf_num] = self

    # -- control requests

    def on_interface_control_xfer(self, stage, request):
        bmRequestType, bRequest, wValue, wIndex, wLength = struct.unpack("BBHHH", request)
        _, req_type, _ = split_bmRequestType(bmRequestType)
        itf = wIndex & 0xFF
        if req_type == _REQ_TYPE_STANDARD:
            if itf != self._itf + 1:
                return False
            if bRequest == _SET_INTERFACE:
                if stage == _STAGE_SETUP:
                    self._set_alt(wValue)
                return True
            if bRequest == _GET_INTERFACE:
                return bytes((self.alt,)) if stage == _STAGE_SETUP else True
            return False
        if req_type != _REQ_TYPE_CLASS or itf != self._itf + 1 or (wIndex >> 8):
            return False  # no controls on the camera or output terminals
        control = wValue >> 8
        if control not in (_VS_PROBE, _VS_COMMIT):
            return False
        if stage == _STAGE_SETUP:
            if bRequest == _SET_CUR:
                self._pending = (control, bytearray(wLength))
                return self._pending[1]
            if bRequest == _GET_CUR:
                return self._probe_values(self._probe_index if control == _VS_PROBE else self._index)
            if bRequest in (_GET_MIN, _GET_DEF):
                return self._probe_values(1)
            if bRequest == _GET_MAX:
                return self._probe_values(len(self.sizes))
            if bRequest == _GET_LEN:
                return struct.pack("<H", _PROBE_LEN)
            if bRequest == _GET_INFO:
                return b"\x03"  # GET and SET supported
            if bRequest == _GET_RES:
                return bytes(_PROBE_LEN)
            return False
        if stage == _STAGE_ACK and self._pending is not None:
            # The host proposes a size by its frame index (byte 3); one rate,
            # so the rest of the answer stays ours, and it reads that back.
            # A commit makes it the size the next session streams.
            which, data = self._pending
            self._pending = None
            index = data[3] if len(data) > 3 else 1
            if not 1 <= index <= len(self.sizes):
                index = 1
            self._probe_index = index
            if which == _VS_COMMIT and not self.alt:
                self._choose(index)
        return True

    def _set_alt(self, alt):
        self.alt = 0    # no frames written while the ring is emptied
        # Every session starts at a frame boundary. A host that stops watching
        # mid-frame leaves that frame's tail in the ring, and the next session
        # would take it for the start of a frame: every frame after it shifted
        # by the same amount, the picture cut into blocks and rearranged. So
        # detach, empty the ring (nothing else reads it once detached), then
        # attach.
        self._usbd.stream(self.ep_in, None)
        scratch = bytearray(512)
        while self.ring.any():
            self.ring.readinto(scratch)
        if alt:
            self._usbd.stream(self.ep_in, self.ring, self.packet, _VARIABLE if self.mjpeg else self.frame_bytes)
        self.alt = alt

    def on_reset(self):
        super().on_reset()
        self.alt = 0

    # -- frames

    @property
    def streaming(self):
        return self.alt != 0

    def wants_frame(self):
        """True when the ring has room for another whole frame."""
        size, waiting, _, _ = self.ring.stats()
        return size - waiting >= self.frame_bytes + (4 if self.mjpeg else 0)

    def write_frame(self, frame):
        """Queue one whole frame (YUY2, or a JPEG up to max_frame bytes);
        False if there's no room for it yet."""
        n = len(frame)
        if self.mjpeg:
            if n > self.frame_bytes:
                raise ValueError("a JPEG of %d bytes is over max_frame, %d" % (n, self.frame_bytes))
            size, waiting, _, _ = self.ring.stats()
            if size - waiting < n + 4:
                return False
            # The length first, then the JPEG: C reads the length, then waits
            # for all of the frame before it sends any of it.
            self.ring.write(struct.pack("<I", n))
            return self.ring.write(frame) == n
        if n != self.frame_bytes:
            raise ValueError("a frame is %d bytes" % self.frame_bytes)
        return self.ring.write(frame) == self.frame_bytes

    def stats(self):
        return self._usbd.stream_stats(self.ep_in), self.ring.stats()
