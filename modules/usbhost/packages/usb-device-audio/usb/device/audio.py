# MicroPython USB audio module: a UAC2 sound card the computer plays to.
# MIT license; Copyright (c) 2026 Brad Barnett
#
#     import usb.device
#     from usb.device.audio import SpeakerInterface
#
#     spk = SpeakerInterface()                 # stereo, 48 kHz, 16-bit
#     usb.device.get().init(spk, builtin_driver=True)
#     while True:
#         spk.service()                        # keeps the clock feedback right
#         n = spk.ring.readinto(buf)           # what the computer is playing
#
# The computer sees a speaker. When it starts playing (selects the streaming
# alternate setting) the samples are moved by machine.USBDevice's streaming
# endpoint, in C, from the USB interrupt into spk.ring, a USBDevice.Ring:
# 16-bit little-endian stereo frames, left then right. Read them out at the
# rate you play them. service() keeps the explicit feedback endpoint telling
# the computer how fast this board consumes, from how full the ring is, so the
# two clocks stay in step without the ring ever running dry or over.
#
# With an audiodsp pump, pass sink=audiopump.Ring(...) and play that ring:
# the samples then go from the USB interrupt into the pump's ring, and Python
# only calls service() now and then.
#
# mute and volume (in 1/256 dB) are what the computer's volume control set.
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
_CUR = const(0x01)
_RANGE = const(0x02)

_CLOCK_ID = const(0x04)
_INPUT_ID = const(0x01)
_FEATURE_ID = const(0x02)
_OUTPUT_ID = const(0x03)

_SAM_FREQ = const(0x01)
_CLOCK_VALID = const(0x02)
_MUTE = const(0x01)
_VOLUME = const(0x02)

_VOLUME_MIN = const(-50 * 256)
_VOLUME_MAX = const(0)
_VOLUME_RES = const(256)


class SpeakerInterface(Interface):
    """A stereo UAC2 speaker. ``ring_bytes`` sizes the ring the samples land
    in; about 50 ms of audio by default."""

    def __init__(self, *, rate=48000, channels=2, sample_bytes=2, ring_bytes=8192, sink=None, interface_str=None):
        super().__init__()
        self.rate = rate
        self.channels = channels
        self.sample_bytes = sample_bytes
        self.frame_bytes = channels * sample_bytes
        self.high_speed = machine.USBDevice().speed() == 2
        # One frame's worth (1 ms at full speed, 125 us at high speed), plus
        # one sample frame of slack for a clock running fast.
        per_frame = rate // (8000 if self.high_speed else 1000)
        self.packet = (per_frame + 1) * self.frame_bytes
        # sink: an audiodsp audiopump.Ring the samples go to instead, from the
        # USB interrupt straight to the pump that plays it (Ring.from_usb).
        self.sink = sink
        self.ring = None if sink is not None else machine.USBDevice.Ring(ring_bytes)
        self.interface_str = interface_str
        self.mute = False
        self.volume = 0
        self.alt = 0
        self.ep_out = self.ep_fb = None
        capacity = sink.level() + sink.space() if sink is not None else self.ring.stats()[0]
        #: The level, in bytes, service() steers the computer's clock to hold.
        self.target = (capacity // 2) // self.frame_bytes * self.frame_bytes
        # Feedback: samples per frame, 10.14 at full speed in 3 bytes, 16.16
        # at high speed in 4.
        if self.high_speed:
            self._nominal = (rate << 16) // 8000
        else:
            self._nominal = (rate << 14) // 1000
        self._fb = self._nominal
        self._usbd = machine.USBDevice()

    # -- descriptors

    def num_itfs(self):
        return 2

    def num_eps(self):
        return 1  # the OUT stream and its feedback IN share one number

    def desc_cfg(self, desc, itf_num, ep_num, strs):
        if self.interface_str:
            strs.append(self.interface_str)
            istr = len(strs) - 1
        else:
            istr = 0
        self.ep_out = ep_num
        self.ep_fb = ep_num | _EP_IN_FLAG
        fb_interval = 4 if self.high_speed else 1  # 1 ms either way
        cs_ac_total = 9 + 8 + 17 + 12 + 18
        desc.extend(bytes((8, 0x0B, itf_num, 2, 0x01, 0x00, 0x20, 0)))  # IAD
        desc.extend(bytes((9, 0x04, itf_num, 0, 0, 0x01, 0x01, 0x20, istr)))  # AC interface
        desc.extend(struct.pack("<BBBHBHB", 9, 0x24, 0x01, 0x0200, 0x01, cs_ac_total, 0))  # AC header
        desc.extend(bytes((8, 0x24, 0x0A, _CLOCK_ID, 0x03, 0x07, _INPUT_ID, 0)))  # clock source
        desc.extend(struct.pack("<BBBBHBBBIBHB", 17, 0x24, 0x02, _INPUT_ID, 0x0101, 0, _CLOCK_ID,
                                self.channels, 0, 0, 0, 0))  # input terminal: USB streaming
        desc.extend(struct.pack("<BBBBHBBBHB", 12, 0x24, 0x03, _OUTPUT_ID, 0x0304, _INPUT_ID, _FEATURE_ID,
                                _CLOCK_ID, 0, 0))  # output terminal: desktop speaker
        fu = struct.pack("<BBBBB", 6 + 4 * (self.channels + 1), 0x24, 0x06, _FEATURE_ID, _INPUT_ID)
        fu += struct.pack("<I", 0x0F) * (self.channels + 1) + b"\x00"  # mute and volume, read-write
        desc.extend(fu)
        desc.extend(bytes((9, 0x04, itf_num + 1, 0, 0, 0x01, 0x02, 0x20, 0)))  # AS alt 0
        desc.extend(bytes((9, 0x04, itf_num + 1, 1, 2, 0x01, 0x02, 0x20, 0)))  # AS alt 1
        desc.extend(struct.pack("<BBBBBBIBIB", 16, 0x24, 0x01, _INPUT_ID, 0, 0x01, 0x01, self.channels, 0, 0))
        desc.extend(bytes((6, 0x24, 0x02, 0x01, self.sample_bytes, self.sample_bytes * 8)))  # Type I format
        desc.extend(struct.pack("<BBBBHB", 7, 0x05, self.ep_out, 0x05, self.packet, 1))  # iso OUT, async
        desc.extend(bytes((8, 0x25, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00)))  # CS iso endpoint
        desc.extend(struct.pack("<BBBBHB", 7, 0x05, self.ep_fb, 0x11, 4 if self.high_speed else 3, fb_interval))
        self._itf = itf_num
        # UAC2 class requests name their target entity in wIndex's high byte,
        # and usb-device's core looks the interface up by the whole wIndex, so
        # each entity gets its own key. (Upstream, the core could look up
        # interface requests by the low byte alone.)
        from . import get

        itfs = get()._itfs
        for entity in (_CLOCK_ID, _INPUT_ID, _FEATURE_ID, _OUTPUT_ID):
            itfs[(entity << 8) | itf_num] = self

    # -- control requests

    def on_interface_control_xfer(self, stage, request):
        bmRequestType, bRequest, wValue, wIndex, wLength = struct.unpack("BBHHH", request)
        _, req_type, _ = split_bmRequestType(bmRequestType)
        itf = wIndex & 0xFF
        to_host = bmRequestType & 0x80
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
        if req_type != _REQ_TYPE_CLASS:
            return False
        entity = wIndex >> 8
        control = wValue >> 8
        if stage == _STAGE_SETUP:
            reply = self._class_request(entity, control, bRequest, to_host)
            if reply is None:
                return False
            if not to_host:
                self._pending = (entity, control, reply)
            return reply
        if stage == _STAGE_ACK and not to_host and self._pending is not None:
            entity, control, data = self._pending
            self._pending = None
            if entity == _FEATURE_ID and control == _MUTE and len(data) >= 1:
                self.mute = bool(data[0])
            elif entity == _FEATURE_ID and control == _VOLUME and len(data) >= 2:
                self.volume = struct.unpack("<h", data)[0]
        return True

    _pending = None

    def _class_request(self, entity, control, bRequest, to_host):
        """The data for a GET, or the buffer a SET's data lands in; None stalls."""
        if entity == _CLOCK_ID:
            if control == _SAM_FREQ and bRequest == _CUR:
                return struct.pack("<I", self.rate) if to_host else bytearray(4)
            if control == _SAM_FREQ and bRequest == _RANGE and to_host:
                return struct.pack("<HIII", 1, self.rate, self.rate, 0)
            if control == _CLOCK_VALID and bRequest == _CUR and to_host:
                return b"\x01"
            return None
        if entity == _FEATURE_ID:
            if control == _MUTE and bRequest == _CUR:
                return bytes((1 if self.mute else 0,)) if to_host else bytearray(1)
            if control == _VOLUME and bRequest == _CUR:
                return struct.pack("<h", self.volume) if to_host else bytearray(2)
            if control == _VOLUME and bRequest == _RANGE and to_host:
                return struct.pack("<Hhhh", 1, _VOLUME_MIN, _VOLUME_MAX, _VOLUME_RES)
            return None
        return None

    def _set_alt(self, alt):
        self.alt = alt
        if alt:
            self._fb = self._nominal
            if self.sink is not None:
                self.sink.from_usb(self.ep_out, self.packet)
            else:
                self._usbd.stream(self.ep_out, self.ring, self.packet)
            self._usbd.feedback(self.ep_fb, self._fb_bytes())
        else:
            if self.sink is not None:
                self.sink.from_usb(self.ep_out, None)
            else:
                self._usbd.stream(self.ep_out, None)
            self._usbd.feedback(self.ep_fb, None)

    def _fb_bytes(self):
        return struct.pack("<I", self._fb)[: 4 if self.high_speed else 3]

    def on_reset(self):
        super().on_reset()
        self.alt = 0

    # -- the clock

    @property
    def streaming(self):
        return self.alt != 0

    def service(self, level=None):
        """Nudge the feedback from how full the ring is: fuller than
        ``target`` asks the computer for fewer samples, emptier for more.
        Call it every few milliseconds from the loop that reads the ring.

        When the samples move on from the ring to somewhere they accumulate
        (an effect chain's own ring), pass that ``level`` in bytes and set
        ``target`` to the level to hold there: the computer's clock is then
        steered by what actually builds up."""
        if not self.alt:
            return
        if level is None:
            level = self.sink.level() if self.sink is not None else self.ring.any()
        error = level - self.target
        # A ring 1/8 off target moves the request by 0.1 %.
        step = (self._nominal * error * 8) // (1000 * max(self.target, self.frame_bytes))
        fb = self._nominal - step
        lo = self._nominal - self._nominal // 100
        hi = self._nominal + self._nominal // 100
        fb = lo if fb < lo else hi if fb > hi else fb
        if fb != self._fb:
            self._fb = fb
            self._usbd.feedback(self.ep_fb, self._fb_bytes())

    def stats(self):
        """(stream packets, bytes, drops), ring (size, waiting, overflows,
        underflows), and the feedback value."""
        ring = self.sink.stats() if self.sink is not None else self.ring.stats()
        return self._usbd.stream_stats(self.ep_out), ring, self._fb
