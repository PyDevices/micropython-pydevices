# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""usb.host.audio and usb.host.video on the isochronous host streams.

The class logic is Python now: the alternate setting (claimed on the host
and sent to the device as SET_INTERFACE), the sample rate, a 2.0 clock's
RANGE, the camera's PROBE and COMMIT. C only moves packets between the
endpoint and a ring. These tests run the real usb.core over a fake _usbif
that answers control requests the way a device does and records the
streams opened, with a Python stand-in for machine.USBDevice.Ring.
"""

import pathlib
import sys
import types
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

from _descriptors import _C920E, _CODEC, _uac2_blob  # noqa: E402

if not hasattr(__import__("time"), "sleep_ms"):
    import time as _time
    _time.sleep_ms = lambda ms: None
    _time.ticks_ms = lambda: 0
    _time.ticks_add = lambda a, b: a + b
    _time.ticks_diff = lambda a, b: a - b


class Ring:
    """machine.USBDevice.Ring's surface: a power-of-two byte ring."""

    def __init__(self, n):
        size = 2
        while size < n:
            size <<= 1
        self.size = size
        self.data = bytearray()

    def any(self):
        return len(self.data)

    def write(self, buf):
        if len(buf) > self.size - len(self.data):
            return 0
        self.data += bytes(buf)
        return len(buf)

    def readinto(self, buf, n=None):
        n = min(len(buf) if n is None else n, len(self.data))
        buf[:n] = self.data[:n]
        del self.data[:n]
        return n

    def stats(self):
        return (self.size, len(self.data), 0, 0)

    def put_frame(self, payload):
        self.data += len(payload).to_bytes(4, "little") + payload


class PumpRing:
    """Stands in for an audiopump.Ring: not a USBDevice.Ring."""


if "micropython" not in sys.modules:
    _mp = types.ModuleType("micropython")
    _mp.const = lambda x: x
    sys.modules["micropython"] = _mp

machine = types.ModuleType("machine")
machine.USBDevice = type("USBDevice", (), {"Ring": Ring})
sys.modules["machine"] = machine


class FakeHost:
    """_usbif's host surface, answering as a device would."""

    def __init__(self):
        self.reset(_CODEC)

    def reset(self, blob, ranges=(48000, 48000, 0), probe_payload=944, refuse_claim=False):
        self.blob = blob
        self.ranges = ranges
        self.probe_payload = probe_payload
        self.refuse_claim = refuse_claim
        self.calls = []
        self.ctrl = []
        self.streams = {}
        self.probe = bytearray(34)
        self.report = None

    # -- usb.core's calls
    def host_start(self, classes, full_speed=False):
        self.calls.append(("start", classes))

    def host_stop(self):
        pass

    def host_devices(self):
        return [(7, 0x08BB, 0x2900, 0, 0, 0, "full")]

    def host_desc(self, dev_id):
        self.desc_reads = getattr(self, "desc_reads", 0) + 1
        return self.blob

    def host_stats(self):
        return (True, 3, 1, 0, 1, 1)

    def host_xfer(self, dev_id, ep, buf, timeout, raise_on_timeout=True):
        self.calls.append(("xfer", ep, timeout, raise_on_timeout))
        if self.report is None:
            if raise_on_timeout:
                raise OSError(110, "ETIMEDOUT")
            return 0
        buf[:len(self.report)] = self.report
        return len(self.report)

    def host_claim(self, dev_id, itf, alt=0):
        if self.refuse_claim:
            raise OSError(16, "EBUSY")
        self.calls.append(("claim", itf, alt))
        return True

    def host_release(self, dev_id, itf):
        self.calls.append(("release", itf))

    def host_ctrl(self, dev_id, setup, buf, timeout):
        rt, req = setup[0], setup[1]
        value = setup[2] | setup[3] << 8
        index = setup[4] | setup[5] << 8
        self.ctrl.append((rt, req, value, index, bytes(buf) if not rt & 0x80 else len(buf)))
        if rt == 0xA1 and req == 0x02:          # UAC2 RANGE
            if self.ranges is None:
                raise OSError(32, "EPIPE")
            n = len(self.ranges) // 3
            ans = n.to_bytes(2, "little") + b"".join(v.to_bytes(4, "little") for v in self.ranges)
            buf[:len(ans)] = ans
            return len(ans)
        if rt == 0x21 and req == 0x01 and value == 0x0100 and len(buf) == 34:   # PROBE
            self.probe[:] = buf
            return len(buf)
        if rt == 0xA1 and req == 0x81:          # GET_CUR PROBE
            ans = bytearray(self.probe)
            ans[18:22] = (320 * 240 * 2).to_bytes(4, "little")
            ans[22:26] = self.probe_payload.to_bytes(4, "little")
            buf[:34] = ans
            return 34
        if rt & 0x80:
            return 0
        return len(buf)

    # -- the streams
    def host_stream(self, dev_id, ep, ring, packet=0, **kw):
        if ring is None:
            self.calls.append(("stream_stop", ep))
            self.streams.pop(ep, None)
            return
        self.calls.append(("stream", ep, packet, kw))
        self.streams[ep] = ring

    def host_stream_stats(self, dev_id, ep):
        return (10, 1000, 1, 2, 3, 4, 5, 6, 7) if ep in self.streams else None

    def host_stream_write(self, dev_id, ep, buf):
        ring = self.streams[ep]
        n = min(len(buf), ring.size - ring.any())
        n -= n % 4
        ring.write(buf[:n])
        return n

    def host_stream_space(self, dev_id, ep):
        ring = self.streams[ep]
        return ring.size - ring.any()

    def host_stream_c_sink(self, dev_id, ep, channels, bits):
        self.calls.append(("c_sink", ep, channels, bits))
        return b"PCMS" + bytes(36)


host = FakeHost()
sys.modules["_usbif"] = host

# Other test modules install fakes of their own under these names; this one
# runs the real packages, so it loads them afresh, and holds on to its own.
for _name in [m for m in sys.modules if m == "usb" or m.startswith("usb.")]:
    del sys.modules[_name]
usb = types.ModuleType("usb")
usb.__path__ = [str(ROOT / "packages" / "usb-core" / "usb")]
sys.modules["usb"] = usb
import usb.core  # noqa: E402
import usb.util  # noqa: E402

usb_host = types.ModuleType("usb.host")
usb_host.__path__ = [str(ROOT / "packages" / "usb-host-audio" / "usb" / "host"),
                     str(ROOT / "packages" / "usb-host-video" / "usb" / "host")]
sys.modules["usb.host"] = usb_host
usb.host = usb_host

from usb.host.audio import Microphone, Speaker  # noqa: E402
from usb.host.video import Camera  # noqa: E402

SET_INTERFACE = (0x01, 0x0B)


def device():
    usb.core.stop()
    return usb.core.find()


class TestSpeaker(unittest.TestCase):
    def test_a_2_0_speaker_asks_its_clock_then_streams_paced(self):
        host.reset(_uac2_blob())
        spk = Speaker(device(), rate=48000, channels=2, bits=16)
        self.assertIn((0xA1, 0x02, 0x0100, 0x0402, 98), host.ctrl)     # RANGE of clock 4, itf 2
        host.ctrl.clear()
        spk.start()
        self.assertIn(("claim", 3, 1), host.calls)
        self.assertEqual(host.ctrl[0][:4], SET_INTERFACE + (1, 3))
        self.assertEqual(host.ctrl[1], (0x21, 0x01, 0x0100, 0x0402, (48000).to_bytes(4, "little")))
        (stream,) = [c for c in host.calls if c[0] == "stream"]
        self.assertEqual(stream, ("stream", 0x03, 196, {"rate": 48000, "frame": 4}))
        self.assertIsInstance(host.streams[0x03], Ring)
        self.assertGreaterEqual(spk.capacity(), 8192)

    def test_write_space_queued_and_stats(self):
        host.reset(_uac2_blob())
        spk = Speaker(device(), rate=48000)
        with spk:
            self.assertEqual(spk.write(bytes(1002)), 1000)    # whole frames
            self.assertEqual(spk.queued(), 1000)
            self.assertEqual(spk.space(), spk.capacity() - 1000)
            self.assertEqual(spk.stats(), (10, 1000, 1, 2, 3, 4))
        self.assertEqual(spk.stats(), (10, 1000, 1, 2, 3, 4))   # the last, after stop
        self.assertEqual(spk.capacity(), 0)
        self.assertEqual(spk.queued(), 0)

    def test_stop_parks_the_device_on_alt_0_and_lets_go(self):
        host.reset(_uac2_blob())
        spk = Speaker(device(), rate=48000)
        spk.start()
        host.ctrl.clear()
        spk.stop()
        self.assertIn(("stream_stop", 0x03), host.calls)
        self.assertEqual(host.ctrl[-1][:4], SET_INTERFACE + (0, 3))
        self.assertEqual(host.calls[-1], ("release", 3))
        self.assertNotIn(0x03, host.streams)

    def test_ring_ms_sizes_the_ring(self):
        host.reset(_uac2_blob())
        spk = Speaker(device(), rate=48000, ring_ms=200)
        spk.start()
        self.assertGreaterEqual(spk.capacity(), 48000 * 4 // 5)
        spk.stop()

    def test_a_1_0_device_takes_its_rate_on_the_endpoint(self):
        host.reset(_CODEC)
        spk = Speaker(device(), rate=48000, channels=2, bits=16)
        host.ctrl.clear()
        spk.start()
        ep = spk.stream.endpoint
        self.assertIn((0x22, 0x01, 0x0100, ep, (48000).to_bytes(3, "little")), host.ctrl)
        stream = [c for c in host.calls if c[0] == "stream"][0]
        self.assertEqual(stream[3], {"rate": 48000, "frame": 4})
        spk.stop()

    def test_a_clock_that_refuses_range_leaves_no_rates(self):
        host.reset(_uac2_blob(), ranges=None)
        with self.assertRaises(ValueError):
            Speaker(device(), rate=44100)

    def test_c_sink_starts_the_stream(self):
        host.reset(_uac2_blob())
        spk = Speaker(device(), rate=48000)
        blob = spk.c_sink()
        self.assertTrue(blob.startswith(b"PCMS"))
        self.assertIn(("c_sink", 0x03, 2, 16), host.calls)
        self.assertIn(0x03, host.streams)
        spk.stop()

    def test_a_refused_claim_names_the_fifo_split(self):
        host.reset(_uac2_blob(), refuse_claim=True)
        spk = Speaker(device(), rate=48000)
        with self.assertRaises(OSError) as e:
            spk.start()
        self.assertIn('classes=("uac",)', str(e.exception))


class TestMicrophone(unittest.TestCase):
    def test_reads_whole_frames_from_the_ring(self):
        host.reset(_CODEC)
        mic = Microphone(device(), rate=48000)
        mic.start()
        ring = host.streams[mic.stream.endpoint]
        self.assertIsInstance(ring, Ring)
        stream = [c for c in host.calls if c[0] == "stream"][0]
        self.assertEqual(stream[3]["rate"], 0)          # capture is not paced
        ring.write(bytes(range(10)) * 10)
        buf = bytearray(64)
        n = mic.read(buf)
        self.assertEqual(n % mic.frame_bytes, 0)
        self.assertEqual(n, 64 - 64 % mic.frame_bytes)
        self.assertEqual(mic.queued(), 100 - n)
        mic.stop()
        self.assertEqual(mic.read(buf), 0)

    def test_a_stereo_only_mic_downmixes_to_mono_when_asked(self):
        host.reset(_uac2_blob(ep=0x83))           # a 2.0 capture stream, stereo only
        mic = Microphone(device(), rate=48000, channels=1)
        self.assertTrue(mic.downmix)
        self.assertEqual((mic.channels, mic.frame_bytes, mic.stream.channels), (1, 2, 2))
        mic.start()
        stream = [c for c in host.calls if c[0] == "stream"][0]
        self.assertEqual(stream[3], {"rate": 0, "frame": 2, "downmix": True})
        ring = host.streams[mic.stream.endpoint]
        ring.write(bytes(range(10)))                # mono frames, as C leaves them
        buf = bytearray(7)
        self.assertEqual(mic.read(buf), 6)         # whole 2-byte frames
        mic.stop()

    def test_stereo_asked_of_a_stereo_mic_is_not_downmixed(self):
        host.reset(_uac2_blob(ep=0x83))
        mic = Microphone(device(), rate=48000, channels=2)
        self.assertFalse(mic.downmix)
        mic.start()
        stream = [c for c in host.calls if c[0] == "stream"][0]
        self.assertNotIn("downmix", stream[3])
        mic.stop()

    def test_feeds_an_audiopump_ring_directly(self):
        host.reset(_CODEC)
        mic = Microphone(device(), rate=48000, bits=16)
        pump = PumpRing()
        mic.start(pump)
        self.assertIs(host.streams[mic.stream.endpoint], pump)
        self.assertEqual(mic.read(bytearray(16)), 0)
        self.assertEqual(mic.queued(), 0)
        mic.stop()


class TestCamera(unittest.TestCase):
    def camera(self, **kw):
        host.reset(_C920E)
        return Camera(device(), **kw)

    def test_negotiates_probe_get_commit(self):
        cam = self.camera(max_size=(320, 240), encoding="mjpeg")
        probes = [c for c in host.ctrl if c[2] in (0x0100, 0x0200)]
        set_probe, get_probe, commit = probes[:3]
        self.assertEqual(set_probe[:3], (0x21, 0x01, 0x0100))
        self.assertEqual(set_probe[4][2:4], bytes((cam._fmt.index, set_probe[4][3])))
        self.assertEqual(get_probe[:3], (0xA1, 0x81, 0x0100))
        self.assertEqual(commit[:3], (0x21, 0x01, 0x0200))
        self.assertEqual(commit[4][22:26], (944).to_bytes(4, "little"))  # the answer, verbatim
        self.assertEqual((cam.encoding, cam.width, cam.height), ("mjpeg", 320, 240))
        self.assertEqual(cam.payload, 944)
        self.assertEqual(cam.frame_bytes, 320 * 240 * 2)

    def test_encoding_matches_whatever_its_case(self):
        cam = self.camera(max_size=(320, 240), encoding="yuy2")
        self.assertEqual(cam.encoding.lower(), "yuy2")
        cam = self.camera(max_size=(320, 240), encoding="YUY2")
        self.assertEqual(cam.encoding.lower(), "yuy2")

    def test_in_limit_skips_modes_too_big_for_the_fifo(self):
        host.reset(_C920E, probe_payload=640)
        with self.assertRaises(ValueError) as e:
            Camera(device(), max_size=(320, 240), in_limit=600)
        self.assertIn("640-byte packets, over in_limit=600", str(e.exception))

    def test_start_claims_the_alt_and_opens_a_video_stream(self):
        cam = self.camera(max_size=(320, 240))
        host.ctrl.clear()
        cam.start()
        a = cam._alt
        self.assertIn(("claim", cam._fmt.interface, a.alt), host.calls)
        self.assertEqual(host.ctrl[0][:4], SET_INTERFACE + (a.alt, cam._fmt.interface))
        stream = [c for c in host.calls if c[0] == "stream"][0]
        self.assertEqual(stream, ("stream", a.endpoint, a.max_packet,
                                  {"video": True, "max_frame": cam.frame_bytes}))
        self.assertGreaterEqual(host.streams[a.endpoint].size, 2 * (cam.frame_bytes + 4))
        cam.stop()

    def test_read_gives_the_newest_whole_frame(self):
        cam = self.camera(max_size=(320, 240))
        buf = bytearray(16)
        self.assertEqual(cam.read(buf), 0)          # not started
        cam.start()
        ring = host.streams[cam._alt.endpoint]
        self.assertEqual(cam.read(buf), 0)
        ring.put_frame(b"old frame")
        ring.put_frame(b"newest")
        self.assertEqual(cam.read(buf), 6)
        self.assertEqual(bytes(buf[:6]), b"newest")
        self.assertEqual(ring.any(), 0)
        # A frame bigger than the buffer is skipped whole, not cut.
        ring.put_frame(b"ok")
        ring.put_frame(b"x" * 600)
        self.assertEqual(cam.read(buf), 2)
        self.assertEqual(bytes(buf[:2]), b"ok")
        self.assertEqual(ring.any(), 0)
        cam.stop()

    def test_stats_keep_stage_1s_order(self):
        cam = self.camera(max_size=(320, 240))
        cam.start()
        # C's (packets, bytes, dropped, starved, errors, empty, frames, torn, big)
        self.assertEqual(cam.stats(), (5, 10, 1000, 1, 6, 7, 3, 4))
        cam.stop()
        self.assertEqual(cam.stats(), (5, 10, 1000, 1, 6, 7, 3, 4))
        self.assertIn(("stream_stop", cam._alt.endpoint), host.calls)
        self.assertEqual(host.ctrl[-1][:4], SET_INTERFACE + (0, cam._fmt.interface))


class TestCoreForPolling(unittest.TestCase):
    """What a no-allocation loop needs from usb.core: a read that answers 0
    on nothing, a descriptor not re-read each call, and the host's state."""

    def setUp(self):
        host.reset(_CODEC)
        self.dev = device()
        self.ep = Microphone(self.dev).stream.endpoint   # any IN endpoint of the device

    def test_raise_on_timeout_false_returns_0_and_keeps_the_transfer(self):
        buf = bytearray(8)
        self.assertEqual(self.dev.read(self.ep, buf, timeout=1, raise_on_timeout=False), 0)
        self.assertEqual(host.calls[-1], ("xfer", self.ep, 1, False))
        host.report = b"\x01\x02"
        self.assertEqual(self.dev.read(self.ep, buf, timeout=1, raise_on_timeout=False), 2)

    def test_the_default_still_raises(self):
        with self.assertRaises(usb.core.USBTimeoutError):
            self.dev.read(self.ep, bytearray(8), timeout=5)

    def test_the_descriptor_is_read_once_not_per_read(self):
        buf = bytearray(8)
        self.dev.read(self.ep, buf, timeout=1, raise_on_timeout=False)
        before = host.desc_reads
        for _ in range(20):
            self.dev.read(self.ep, buf, timeout=1, raise_on_timeout=False)
        self.assertEqual(host.desc_reads, before)

    def test_running_and_stats(self):
        self.assertTrue(usb.core.running())
        st = usb.core.stats()
        self.assertEqual((st["running"], st["attaches"], st["detaches"], st["devices"]), (True, 3, 1, 1))
        usb.core.stop()
        self.assertFalse(usb.core.running())
        self.assertFalse(usb.core.stats()["running"])


if __name__ == "__main__":
    unittest.main()
