# MicroPython USB audio host: a USB speaker or microphone on the host port.
# MIT license; Copyright (c) 2026 Brad Barnett
#
#     import usb.core
#     from usb.host.audio import Speaker
#
#     usb.core.start(classes=("uac",))
#     spk = Speaker(usb.core.find(custom_match=Speaker.match))
#     print(spk.rate, spk.channels, spk.bits)     # what it opened
#     spk.start()
#     spk.write(pcm)                  # little-endian PCM, whole frames
#
# Any class-compliant device, USB Audio 1.0 or 2.0: a headset, a USB DAC, or
# a PyDevices board running usb-device-audio. Its descriptors are read here,
# a stream is chosen (the rate, channels and bits you ask for, or its best
# offer), a 2.0 device's clock is asked its rates, and the samples move over
# isochronous transfers in C, through a ring between your writes and the bus.
# A microphone can also feed audiodsp's pump straight from the bus:
# mic.start(ring) with an audiopump.Ring.
#
# Start the host with classes=("uac",): the controller's FIFO split is fixed
# while it runs, and one set up for video has no room for a speaker's
# periodic OUT packets.
import _usbif

from machine import USBDevice

from . import _uac

_SET_CUR = 0x01
_RANGE = 0x02
_SAM_FREQ = 0x0100          # SAMPLING_FREQ_CONTROL << 8, both versions
_CTRL_MS = 3000
_MAX_SUBRANGES = 8
_DEFAULT_RING = 8192        # about 43 ms of 48 kHz stereo


def _clock_ranges(dev, control, clock):
    """A 2.0 clock source's RANGE answer, as flat (min, max, res) triplets."""
    buf = bytearray(2 + 12 * _MAX_SUBRANGES)
    n = dev.ctrl_transfer(0xA1, _RANGE, _SAM_FREQ, (clock << 8) | control, buf, _CTRL_MS)
    if n < 2:
        raise OSError("short RANGE answer")
    out = []
    for i in range(min(buf[0] | buf[1] << 8, _MAX_SUBRANGES)):
        at = 2 + 12 * i
        if at + 12 > n:
            break
        for k in range(3):
            out.append(int.from_bytes(buf[at + 4 * k:at + 4 * k + 4], "little"))
    return out


def _streams(dev):
    """The device's streams, a 2.0 stream's rates asked of its clock."""
    out = []
    asked = {}
    at = _uac.STREAM_FIELDS.index("rates")
    for s in _uac.streams(dev.configuration_descriptor()):
        if s.rates or not s.clock:
            out.append(s)
            continue
        key = (s.control, s.clock)
        if key not in asked:
            try:
                asked[key] = _uac.rates_from_ranges(_clock_ranges(dev, s.control, s.clock))
            except OSError:
                asked[key] = ()
        fields = list(s)
        fields[at] = asked[key]
        out.append(_uac.UacStream(*fields))
    return out


class _Stream:
    _direction = None

    @classmethod
    def match(cls, dev):
        """For usb.core.find(custom_match=...): a device with a stream this
        way."""
        return any(s.direction == cls._direction for s in _uac.streams(dev.configuration_descriptor()))

    def __init__(self, dev, *, rate=None, channels=None, bits=None, ring_ms=None):
        """Choose the stream matching what's given (None: the device's best
        offer). ``ring_ms`` sizes the ring between Python and the bus; the
        default is 8 KB, about 43 ms of 48 kHz stereo, which wants a caller
        that services it often."""
        if dev is None:
            raise ValueError("no device")
        self.dev = dev
        self._id = dev._id
        found = _streams(dev)
        s = _uac.choose(found, self._direction, rate, channels, bits)
        self.downmix = False
        if s is None and channels == 1 and self._direction == _uac.IN and bits in (None, 16):
            # A stereo-only microphone (a webcam's, often) for a mono
            # consumer: take its 16-bit stereo, and C averages each frame to
            # one sample as the packets land.
            s = _uac.choose(found, self._direction, rate, 2, 16)
            self.downmix = s is not None
        if s is None:
            raise ValueError("no %s stream matching rate=%s channels=%s bits=%s; it offers: %s" % (
                self._direction, rate, channels, bits, ", ".join(_uac.describe(x) for x in found)))
        self.stream = s
        self.rate = rate or (max(s.rates) if s.rates else 48000)
        self.channels = 1 if self.downmix else s.channels
        self.bits = s.bits
        self.frame_bytes = self.channels * s.frame_bytes
        self._ring_bytes = _DEFAULT_RING
        if ring_ms:
            self._ring_bytes = max(1, (self.rate * ring_ms + 999) // 1000) * self.frame_bytes
        self._ring = None
        self._open = False
        self._last = (0, 0, 0, 0, 0, 0)

    def _set_rate(self):
        # Advisory: a device with one fixed rate may refuse (stall) it.
        s = self.stream
        try:
            if s.clock:     # 2.0: CUR on the clock source, four bytes
                self.dev.ctrl_transfer(0x21, _SET_CUR, _SAM_FREQ, (s.clock << 8) | s.control,
                                       self.rate.to_bytes(4, "little"), _CTRL_MS)
            else:           # 1.0: an endpoint control, three bytes
                self.dev.ctrl_transfer(0x22, _SET_CUR, _SAM_FREQ, s.endpoint,
                                       self.rate.to_bytes(3, "little"), _CTRL_MS)
        except OSError:
            pass

    def _park(self):
        # Back to alternate 0, the zero-bandwidth setting, and let go: the
        # device stops streaming and stops reserving the bus.
        itf = self.stream.interface
        try:
            self.dev.set_interface_altsetting(itf, 0)
        except OSError:
            pass
        try:
            self.dev._release(itf)
        except OSError:
            pass

    def _start(self, ring, rate):
        s = self.stream
        try:
            self.dev.set_interface_altsetting(s.interface, s.alt)
        except OSError as e:
            raise OSError("the interface claim was refused (%s); for a speaker, start the host with "
                          "usb.core.start(classes=(\"uac\",))" % e)
        self._set_rate()
        try:
            if self.downmix:
                _usbif.host_stream(self._id, s.endpoint, ring, s.max_packet & 0x7FF, rate=rate,
                                   frame=self.frame_bytes, downmix=True)
            else:
                _usbif.host_stream(self._id, s.endpoint, ring, s.max_packet & 0x7FF, rate=rate,
                                   frame=self.frame_bytes)
        except BaseException:
            self._park()
            raise
        self._ring = ring
        self._open = True

    def start(self):
        if not self._open:
            self._start(USBDevice.Ring(max(self._ring_bytes, 2 * (self.stream.max_packet & 0x7FF))), 0)

    def stop(self):
        if self._open:
            self._last = self.stats()
            _usbif.host_stream(self._id, self.stream.endpoint, None)
            self._open = False
            self._ring = None
            self._park()

    def capacity(self):
        """The ring's size in bytes; 0 while stopped."""
        return self._ring.stats()[0] if self._open and isinstance(self._ring, USBDevice.Ring) else 0

    def stats(self):
        """(packets, bytes, dropped, starved, errors, empty): ``dropped``
        means Python was late, ``starved`` that the ring was empty when the
        bus asked, ``errors`` bad packets on the bus. After stop(), the
        stream's last."""
        if not self._open:
            return self._last
        st = _usbif.host_stream_stats(self._id, self.stream.endpoint)
        return tuple(st[:6]) if st else self._last

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()


class Speaker(_Stream):
    """A USB speaker, headset or DAC: PCM written here plays."""

    _direction = _uac.OUT

    def start(self):
        if not self._open:
            self._start(USBDevice.Ring(max(self._ring_bytes, 2 * (self.stream.max_packet & 0x7FF))), self.rate)

    def write(self, buf):
        """Queue PCM; returns the bytes taken, whole frames, maybe fewer
        than given when the ring is full."""
        return _usbif.host_stream_write(self._id, self.stream.endpoint, buf)

    def space(self):
        """Bytes a write can take right now."""
        return _usbif.host_stream_space(self._id, self.stream.endpoint)

    def queued(self):
        """Bytes written and not yet sent."""
        return self._ring.any() if self._open else 0

    def c_sink(self):
        """The playing stream as a C sink (pcm_c_sink.h), by value: for
        another native module to write PCM from its own task, the
        interpreter out of the path. Starts the stream if it isn't running.
        The sink dies when the stream stops; ask again after a restart."""
        self.start()
        return _usbif.host_stream_c_sink(self._id, self.stream.endpoint, self.channels, self.bits)


class Microphone(_Stream):
    """A USB microphone or audio interface input: PCM read here.

    ``channels=1`` on a microphone that only offers two (16-bit) gives one
    anyway: each frame arrives as the mean of its two samples, and
    ``downmix`` is True."""

    _direction = _uac.IN

    def start(self, ring=None):
        """Start recording. ``ring``, an audiodsp ``audiopump.Ring`` (16-bit,
        at this stream's rate and channels), takes the samples straight from
        the bus for the pump to play; read() then has nothing."""
        if not self._open:
            if ring is None:
                ring = USBDevice.Ring(max(self._ring_bytes, 2 * (self.stream.max_packet & 0x7FF)))
            elif self.bits != 16:
                raise ValueError("an audiopump.Ring takes 16-bit audio; this stream is %d-bit" % self.bits)
            self._start(ring, 0)

    def read(self, buf):
        """Recorded PCM into buf; returns the bytes read, 0 when none yet."""
        r = self._ring
        if not self._open or not isinstance(r, USBDevice.Ring):
            return 0
        n = min(len(buf), r.any())
        n -= n % self.frame_bytes
        return r.readinto(buf, n) if n else 0

    def queued(self):
        """Bytes recorded and not yet read."""
        r = self._ring
        return r.any() if self._open and isinstance(r, USBDevice.Ring) else 0
