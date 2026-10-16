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
#
# Start the host with classes=("uac",): the controller's FIFO split is fixed
# while it runs, and one set up for video has no room for a speaker's
# periodic OUT packets.
import _usbif

from . import _uac


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
                asked[key] = _uac.rates_from_ranges(_usbif.host_uac_clock_ranges(dev._id, s.control, s.clock))
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
        if s is None:
            raise ValueError("no %s stream matching rate=%s channels=%s bits=%s; it offers: %s" % (
                self._direction, rate, channels, bits, ", ".join(_uac.describe(x) for x in found)))
        self.stream = s
        self.rate = rate or (max(s.rates) if s.rates else 48000)
        self.channels = s.channels
        self.bits = s.bits
        self.frame_bytes = s.channels * s.frame_bytes
        self._ring = 0
        if ring_ms:
            self._ring = max(1, (self.rate * ring_ms + 999) // 1000) * self.frame_bytes
        self._open = False

    def start(self):
        if not self._open:
            s = self.stream
            _usbif.host_uac_open(self._id, s.interface, s.alt, s.endpoint, s.max_packet, self.rate,
                                 s.clock, s.control, self.frame_bytes, self._ring)
            self._open = True

    def stop(self):
        if self._open:
            _usbif.host_uac_close(self.stream.endpoint)
            self._open = False

    def capacity(self):
        """The ring's size in bytes; 0 while stopped."""
        return max(0, _usbif.host_uac_capacity(self.stream.endpoint))

    def stats(self):
        """(packets, bytes, dropped, starved, errors, empty): ``dropped``
        means Python was late, ``starved`` that the ring was empty when the
        bus asked, ``errors`` bad packets on the bus."""
        return _usbif.host_uac_stats(self.stream.endpoint)

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()


class Speaker(_Stream):
    """A USB speaker, headset or DAC: PCM written here plays."""

    _direction = _uac.OUT

    def write(self, buf):
        """Queue PCM; returns the bytes taken, whole frames, maybe fewer
        than given when the ring is full."""
        return _usbif.host_uac_write(buf)

    def space(self):
        """Bytes a write can take right now."""
        return _usbif.host_uac_space(self.stream.endpoint)

    def queued(self):
        """Bytes written and not yet sent."""
        return _usbif.host_uac_queued(self.stream.endpoint)


class Microphone(_Stream):
    """A USB microphone or audio interface input: PCM read here."""

    _direction = _uac.IN

    def read(self, buf):
        """Recorded PCM into buf; returns the bytes read, 0 when none yet."""
        return _usbif.host_uac_read(buf)

    def queued(self):
        """Bytes recorded and not yet read."""
        return _usbif.host_uac_queued(self.stream.endpoint)
