# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""The UAC and UVC descriptor parsers in usb.host.audio and usb.host.video.

``usb/host/_uac.py`` and ``usb/host/_uvc.py`` walk a configuration blob to
learn what an audio or video device offers before anything streams. They
import nothing from usb.core, so they're loaded here straight from their
files: no fake host is needed, and nothing goes into ``sys.modules`` that
another test module's ``usb`` package would collide with.

The fixtures are synthetic blobs built byte by byte, and descriptors captured
from real devices (``_descriptors.py`` holds the ones other tests share).
"""

import importlib.util
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

from _descriptors import _C920E, _CODEC, _uac2_blob  # noqa: E402


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / "packages" / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


uac = _load("_uac_under_test", "usb-host-audio/usb/host/_uac.py")
uvc = _load("_uvc_under_test", "usb-host-video/usb/host/_uvc.py")


def _uac_blob(rates=(48000,), channels=1, bits=16, ep=0x81, attrs=0x05,
              max_packet=192, extra_alt=None):
    """A realistic UAC 1.0 configuration blob.

    Assembled byte by byte rather than captured, so the test exercises the
    real descriptor layout including the parts a naive reader gets wrong: the
    three-byte sample rates, the audio endpoint's 9-byte form, and alt 0
    existing with no endpoint at all.
    """
    def itf(number, alt, n_eps, subclass):
        return bytes([9, 0x04, number, alt, n_eps, 0x01, subclass, 0x00, 0x00])

    def rate_bytes(value):
        return bytes([value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF])

    parts = [bytes([9, 0x02, 0, 0, 2, 1, 0, 0x80, 50])]        # CONFIGURATION
    parts.append(itf(0, 0, 0, 0x01))                            # AudioControl
    parts.append(bytes([9, 0x24, 0x01, 0x00, 0x01, 9, 0, 1, 1]))  # CS header
    parts.append(itf(1, 0, 0, 0x02))                            # AS alt 0: silent
    parts.append(itf(1, 1, 1, 0x02))                            # AS alt 1
    parts.append(bytes([7, 0x24, AS_GENERAL_SUBTYPE, 0x02, 1, 0x01, 0x00]))
    fmt = bytes([8 + 3 * len(rates), 0x24, 0x02, 0x01, channels,
                 bits // 8, bits, len(rates)])
    for value in rates:
        fmt += rate_bytes(value)
    parts.append(fmt)
    parts.append(bytes([9, 0x05, ep, attrs, max_packet & 0xFF,
                        (max_packet >> 8) & 0xFF, 0x01, 0x00, 0x00]))
    if extra_alt:
        parts.extend(extra_alt)
    blob = b"".join(parts)
    return blob[:2] + bytes([len(blob) & 0xFF, (len(blob) >> 8) & 0xFF]) + blob[4:]


AS_GENERAL_SUBTYPE = 0x01


class TestUac2DescriptorParsing(unittest.TestCase):
    """USB Audio 2.0 reading: the layouts differ from 1.0 in every descriptor
    the parser touches, and the rates are not in the descriptors at all.
    Found the hard way (usbif#28): the S3 hosting the P4, itself a 2.0
    device, read it as "? Hz x 0ch x 0bit"."""

    def test_channels_bits_and_endpoint_come_from_the_2_0_layouts(self):
        (stream,) = uac.streams(_uac2_blob())
        self.assertEqual(stream.direction, uac.OUT)
        self.assertEqual(stream.channels, 2)
        self.assertEqual(stream.bits, 16)
        self.assertEqual(stream.frame_bytes, 2)
        self.assertEqual(stream.endpoint, 0x03)
        self.assertEqual(stream.max_packet, 196)
        self.assertEqual(stream.sync, "async")

    def test_rates_are_empty_and_the_clock_is_named(self):
        (stream,) = uac.streams(_uac2_blob())
        self.assertEqual(stream.rates, ())
        self.assertEqual(stream.clock, 0x04)
        self.assertEqual(stream.control, 2)

    def test_the_first_clock_serves_when_no_terminal_names_one(self):
        (stream,) = uac.streams(_uac2_blob(name_clock_in_terminal=False))
        self.assertEqual(stream.clock, 0x04)

    def test_a_1_0_stream_carries_no_clock(self):
        (stream,) = uac.streams(_uac_blob())
        self.assertEqual((stream.clock, stream.control), (0, 0))

    def test_the_feedback_endpoint_is_not_a_stream(self):
        found = uac.streams(_uac2_blob())
        self.assertEqual(len(found), 1)

    def test_rates_from_a_clock_range_answer(self):
        # One discrete rate, the way usbif's own device answers.
        self.assertEqual(uac.rates_from_ranges((48000, 48000, 0)), (48000,))
        # Two discrete subranges.
        self.assertEqual(uac.rates_from_ranges((44100, 44100, 0, 48000, 48000, 0)),
                         (44100, 48000))
        # A continuous range expands to the standard rates it contains.
        self.assertEqual(uac.rates_from_ranges((8000, 48000, 1)),
                         (8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000))
        # A stepped range keeps only rates on the step.
        self.assertEqual(uac.rates_from_ranges((8000, 48000, 8000)), (8000, 16000, 24000, 32000, 48000))
        # Empty and short answers give nothing.
        self.assertEqual(uac.rates_from_ranges(()), ())
        self.assertEqual(uac.rates_from_ranges((48000, 48000)), ())


class TestUacDescriptorParsing(unittest.TestCase):
    """UAC 1.0 descriptor reading -- how a host learns what a device can do.

    A UAC device announces its formats in class-specific descriptors
    interleaved between its interfaces, so choosing a format means walking the
    whole configuration blob and then choosing an alternate setting. These
    assertions cover the parts that fail quietly rather than loudly.
    """

    def test_a_simple_microphone_parses(self):
        found = uac.streams(_uac_blob())
        self.assertEqual(len(found), 1)
        stream = found[0]
        self.assertEqual(stream.direction, uac.IN)
        self.assertEqual(stream.rates, (48000,))
        self.assertEqual(stream.channels, 1)
        self.assertEqual(stream.bits, 16)
        self.assertEqual(stream.endpoint, 0x81)
        self.assertEqual(stream.max_packet, 192)
        self.assertEqual(stream.sync, "async")

    def test_sample_rates_are_three_byte_little_endian(self):
        # A four-byte read here returns a plausible-looking wrong number
        # rather than failing, which is the worst kind of parser bug.
        (stream,) = uac.streams(_uac_blob(rates=(44100, 48000, 96000)))
        self.assertEqual(stream.rates, (44100, 48000, 96000))

    def test_alt_zero_is_not_offered_as_a_stream(self):
        # By specification alt 0 has no endpoint and exists so a device can be
        # configured while using no bandwidth. Offering it would be offering a
        # format that is silent by design.
        for stream in uac.streams(_uac_blob()):
            self.assertNotEqual(stream.alt, 0)

    def test_a_feedback_endpoint_is_not_mistaken_for_audio(self):
        # Feedback endpoints carry rate corrections, not samples. Treating one
        # as a stream would produce a device that "works" and is silent.
        found = uac.streams(_uac_blob(ep=0x82, attrs=0x11))  # isoc | feedback
        self.assertEqual(found, ())

    def test_a_bulk_endpoint_is_ignored(self):
        self.assertEqual(uac.streams(_uac_blob(attrs=0x02)), ())

    def test_an_output_endpoint_is_reported_as_out(self):
        (stream,) = uac.streams(_uac_blob(ep=0x02))
        self.assertEqual(stream.direction, uac.OUT)

    def test_has_audio_detects_a_streaming_interface(self):
        self.assertTrue(uac.has_audio(_uac_blob()))
        self.assertFalse(uac.has_audio(bytes([9, 0x02, 9, 0, 0, 1, 0, 0x80, 50])))

    def test_choose_prefers_the_best_offer_when_asked_for_nothing(self):
        found = uac.streams(_uac_blob(rates=(8000, 48000)))
        picked = uac.choose(found, uac.IN)
        self.assertEqual(max(picked.rates), 48000)

    def test_choose_filters_on_what_the_caller_asked_for(self):
        found = uac.streams(_uac_blob(rates=(44100, 48000)))
        self.assertIsNotNone(uac.choose(found, uac.IN, rate=44100))
        self.assertIsNone(uac.choose(found, uac.IN, rate=192000))
        self.assertIsNone(uac.choose(found, uac.OUT))

    def test_a_truncated_blob_stops_rather_than_looping(self):
        # Real hardware returns short and padded descriptors. A parser that
        # hangs on one is worse than a parser that stops early.
        blob = _uac_blob()
        self.assertEqual(uac.streams(blob[:len(blob) // 2]), ())
        self.assertEqual(list(uac.descriptors(b"\x00\x00")), [])
        self.assertEqual(list(uac.descriptors(b"")), [])

    def test_describe_names_the_essentials(self):
        (stream,) = uac.streams(_uac_blob())
        text = uac.describe(stream)
        for fragment in ("48000", "1ch", "16bit", "in"):
            self.assertIn(fragment, text)


# --- Real descriptors, captured from hardware -----------------------------
#
# A synthetic blob tests the parser against the author's understanding of the
# spec. These test it against what devices actually send, which is not always
# the same thing -- and they cost nothing to keep, so a future change that
# breaks a real device breaks a test instead.
#
# _CODEC and _C920E come from _descriptors.py.


# Captured 2026-09-03 from real hardware (08bb:2902), 153 bytes.
_PNP_MIC = bytes.fromhex(
    "0902990003010080320904000000010100000924010001370001010c24020201020001010000"
    "0009240307010100080007240508010a000924060a02014300000924060d0201030000090401"
    "000001020000090401010101020000072401070101000e2402010102100280bb0044ac000905"
    "8209640001000007250101000000090402000103000000092100010001223c00070587030400"
    "02"
)


class TestUacRealDescriptors(unittest.TestCase):
    """The parser against two real USB audio devices.

    A Burr-Brown/TI CODEC with 24 alternate settings across both directions,
    and a single-format C-Media microphone. Between them they cover the shapes
    a synthetic fixture is least likely to imagine: many alts on one
    interface, both directions on one device, all three synchronisation types,
    and 8-bit as well as 16-bit formats.
    """

    def test_the_codec_offers_both_directions(self):
        found = uac.streams(_CODEC)
        self.assertTrue(any(s.direction == uac.OUT for s in found))
        self.assertTrue(any(s.direction == uac.IN for s in found))

    def test_the_codec_output_is_found_at_cd_quality_and_better(self):
        found = uac.streams(_CODEC)
        best = uac.choose(found, uac.OUT)
        self.assertEqual(best.channels, 2)
        self.assertEqual(best.bits, 16)
        self.assertIn(48000, best.rates)
        self.assertIn(44100, best.rates)

    def test_every_codec_stream_is_isochronous_with_a_real_endpoint(self):
        for stream in uac.streams(_CODEC):
            self.assertIsNotNone(stream.endpoint)
            self.assertGreater(stream.max_packet, 0)
            self.assertIn(stream.sync, ("async", "adaptive", "sync", "none"))

    def test_the_codec_input_and_output_use_different_interfaces(self):
        # Two directions on one device means two AudioStreaming interfaces,
        # and a parser that keyed on interface alone would collapse them.
        found = uac.streams(_CODEC)
        ins = {s.interface for s in found if s.direction == uac.IN}
        outs = {s.interface for s in found if s.direction == uac.OUT}
        self.assertTrue(ins)
        self.assertTrue(outs)
        self.assertFalse(ins & outs)

    def test_alt_settings_are_distinct_and_none_is_zero(self):
        for interface in (1, 2):
            alts = [s.alt for s in uac.streams(_CODEC) if s.interface == interface]
            self.assertEqual(len(alts), len(set(alts)))
            self.assertNotIn(0, alts)

    def test_the_microphone_is_input_only_at_one_format(self):
        found = uac.streams(_PNP_MIC)
        self.assertEqual(len(found), 1)
        stream = found[0]
        self.assertEqual(stream.direction, uac.IN)
        self.assertEqual(stream.channels, 1)
        self.assertEqual(stream.bits, 16)
        self.assertEqual(set(stream.rates), {48000, 44100})
        self.assertIsNone(uac.choose(found, uac.OUT))

    def test_both_devices_are_recognised_as_audio(self):
        self.assertTrue(uac.has_audio(_CODEC))
        self.assertTrue(uac.has_audio(_PNP_MIC))

    def test_choosing_an_unavailable_rate_returns_none_rather_than_guessing(self):
        # The mic offers 44100 and 48000. Asking for 96000 must fail rather
        # than silently returning the nearest, which would start a stream at a
        # rate the caller did not ask for.
        found = uac.streams(_PNP_MIC)
        self.assertIsNone(uac.choose(found, uac.IN, rate=96000))
        self.assertIsNotNone(uac.choose(found, uac.IN, rate=44100))


def _uvc_blob():
    """A synthetic UVC configuration descriptor, built rather than captured.

    Built because the camera has not reached the bench yet, and a parser with
    no test is worth less than no parser. Its shape follows the layout every
    UVC 1.1 camera uses -- formats and frames on alt 0, bandwidth tiers on
    alts 1..n -- so the structural claims this exercises (frames attach to the
    preceding format, alts carry only packet sizes) hold for a real device
    too. It is NOT a substitute for a real capture: byte-level surprises are
    exactly what real hardware supplies, and this cannot.
    """
    def desc(*parts):
        body = bytes(parts)
        return bytes((len(body) + 1,)) + body

    def le16(v):
        return (v & 0xFF, (v >> 8) & 0xFF)

    def le32(v):
        return (v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF)

    def frame(subtype, index, w, h, intervals, max_bytes):
        return desc(0x24, subtype, index, 0x00, *le16(w), *le16(h),
                    *le32(1000000), *le32(20000000), *le32(max_bytes),
                    *le32(intervals[0]), len(intervals),
                    *[b for i in intervals for b in le32(i)])

    def interface(num, alt, nep, cls, sub):
        return desc(0x04, num, alt, nep, cls, sub, 0x00, 0x00)

    def endpoint(addr, attrs, mps, ival):
        return desc(0x05, addr, attrs, *le16(mps), ival)

    parts = [
        desc(0x02, 0x00, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32),   # CONFIGURATION
        interface(0, 0, 1, 0x0E, 0x01),                          # VideoControl
        desc(0x24, 0x01, 0x00, 0x01, 0x4D, 0x00, *le32(15000000), 0x00, 0x00),
        interface(1, 0, 0, 0x0E, 0x02),                          # VS alt 0
        desc(0x24, 0x01, 0x02, *le16(0x00DA), 0x81, 0x00, 0x02,
             0x00, 0x00, 0x00, 0x01, 0x00, 0x00),                # VS_INPUT_HEADER
        desc(0x24, 0x06, 0x01, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00),
        frame(0x07, 1, 640, 480, [333333, 666666], 614400),
        frame(0x07, 2, 320, 240, [333333], 153600),
        desc(0x24, 0x04, 0x02, 0x01,
             0x59, 0x55, 0x59, 0x32, 0x00, 0x00, 0x10, 0x00,     # "YUY2" GUID
             0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
             0x10, 0x01, 0x00, 0x00, 0x00, 0x00),
        frame(0x05, 1, 320, 240, [333333], 153600),
        interface(1, 1, 1, 0x0E, 0x02),                          # bandwidth tiers
        endpoint(0x81, 0x05, 192, 1),
        interface(1, 2, 1, 0x0E, 0x02),
        endpoint(0x81, 0x05, 512, 1),
        interface(1, 3, 1, 0x0E, 0x02),
        endpoint(0x81, 0x05, 1023, 1),
    ]
    return b"".join(parts)


class TestUvcDescriptors(unittest.TestCase):
    def setUp(self):
        self.uvc = uvc
        self.blob = _uvc_blob()

    def test_recognises_a_video_streaming_interface(self):
        self.assertTrue(self.uvc.has_video(self.blob))
        self.assertFalse(self.uvc.has_video(b""))

    def test_formats_carry_their_own_frames(self):
        # The association is positional in UVC -- a frame descriptor has no
        # back-reference to its format -- so getting this wrong silently
        # attributes 640x480 to the wrong encoding.
        found = self.uvc.formats(self.blob)
        self.assertEqual([f.encoding for f in found], ["mjpeg", "YUY2"])
        self.assertEqual([len(f.frames) for f in found], [2, 1])
        self.assertEqual(found[1].bits_per_pixel, 16)

    def test_frame_sizes_and_rates(self):
        mjpeg = self.uvc.formats(self.blob)[0]
        self.assertEqual([(f.width, f.height) for f in mjpeg.frames],
                         [(640, 480), (320, 240)])
        self.assertEqual(len(mjpeg.frames[0].intervals), 2)
        self.assertAlmostEqual(self.uvc.fps(mjpeg.frames[0].intervals[0]),
                               30.0, places=3)
        self.assertAlmostEqual(self.uvc.fps(mjpeg.frames[0].intervals[1]),
                               15.0, places=3)

    def test_frames_are_not_read_from_the_bandwidth_alts(self):
        # Formats live on alt 0 only. A parser that ignored the alt would
        # find nothing extra here, so assert the count is exactly right.
        self.assertEqual(sum(len(f.frames)
                             for f in self.uvc.formats(self.blob)), 3)

    def test_alt_settings_are_bandwidth_tiers_on_one_endpoint(self):
        alts = self.uvc.alt_settings(self.blob)
        self.assertEqual([a.alt for a in alts], [1, 2, 3])
        self.assertEqual({a.endpoint for a in alts}, {0x81})
        self.assertEqual([a.max_packet for a in alts], [192, 512, 1023])
        self.assertEqual({a.transfer for a in alts}, {"isoc"})
        self.assertEqual({a.per_frame for a in alts}, {1})

    def test_alt_zero_contributes_no_endpoint(self):
        self.assertNotIn(0, [a.alt for a in self.uvc.alt_settings(self.blob)])

    def test_choose_prefers_the_largest_frame_then_the_fastest_rate(self):
        found = self.uvc.formats(self.blob)
        picked = self.uvc.choose(found)
        self.assertIsNotNone(picked)
        fmt, frame, interval = picked
        self.assertEqual((frame.width, frame.height), (640, 480))
        # 640x480 offers 30 and 15 fps; the faster one wins at equal size.
        self.assertAlmostEqual(self.uvc.fps(interval), 30.0, places=3)

    def test_choose_honours_an_explicit_encoding(self):
        found = self.uvc.formats(self.blob)
        picked = self.uvc.choose(found, encoding="YUY2")
        self.assertIsNotNone(picked)
        self.assertEqual(picked[0].encoding, "YUY2")
        self.assertEqual((picked[1].width, picked[1].height), (320, 240))

    def test_choose_honours_a_minimum_rate(self):
        found = self.uvc.formats(self.blob)
        self.assertIsNotNone(self.uvc.choose(found, min_fps=25))
        self.assertIsNone(self.uvc.choose(found, min_fps=60))

    def test_choose_returns_none_when_nothing_matches(self):
        found = self.uvc.formats(self.blob)
        self.assertIsNone(self.uvc.choose(found, width=1920))
        self.assertIsNone(self.uvc.choose(found, encoding="H264"))

    def test_alt_is_selected_from_the_negotiated_payload_not_a_guess(self):
        # The regression this guards: selecting the alt from
        # dwMaxVideoFrameBufferSize refuses every mode on a full-speed bus,
        # because that field is a worst case. A real device answers PROBE
        # with a payload size, and that is what picks the tier.
        alts = self.uvc.alt_settings(self.blob)
        self.assertEqual(self.uvc.alt_for_payload(alts, 100).max_packet, 192)
        self.assertEqual(self.uvc.alt_for_payload(alts, 500).max_packet, 512)
        self.assertEqual(self.uvc.alt_for_payload(alts, 1000).max_packet, 1023)
        self.assertIsNone(self.uvc.alt_for_payload(alts, 4096))

    def test_required_bytes_scales_with_rate(self):
        frame = self.uvc.formats(self.blob)[0].frames[0]
        at30 = self.uvc.required_packet_bytes(frame, 333333)
        at15 = self.uvc.required_packet_bytes(frame, 666666)
        self.assertGreater(at30, at15)
        self.assertAlmostEqual(at30 / at15, 2.0, places=1)

    def test_describe_is_a_single_line(self):
        fmt = self.uvc.formats(self.blob)[0]
        text = self.uvc.describe(fmt, fmt.frames[0])
        self.assertIn("640x480", text)
        self.assertIn("mjpeg", text)
        self.assertNotIn("\n", text)


class TestUvcRealCamera(unittest.TestCase):
    """The UVC parser against a descriptor a real camera actually sent."""

    def setUp(self):
        self.uvc = uvc

    def test_reads_exactly_the_two_formats_the_camera_offers(self):
        # The regression: VideoControl's VC_SELECTOR_UNIT / VC_PROCESSING_UNIT
        # / VC_EXTENSION_UNIT are subtypes 4, 5 and 6 -- the same numbers
        # VideoStreaming uses for FORMAT_UNCOMPRESSED, FRAME_UNCOMPRESSED and
        # FORMAT_MJPEG. Parsing without checking the interface subclass turned
        # this camera's six extension units into six phantom MJPEG formats
        # with no frames.
        found = self.uvc.formats(_C920E)
        self.assertEqual([f.encoding for f in found], ["YUY2", "mjpeg"])
        self.assertEqual([len(f.frames) for f in found], [19, 17])
        self.assertTrue(all(f.frames for f in found))

    def test_offers_the_modes_the_camera_is_sold_on(self):
        found = self.uvc.formats(_C920E)
        picked = self.uvc.choose(found, width=1280, height=720,
                                 encoding="mjpeg", min_fps=30)
        self.assertIsNotNone(picked)
        fmt, frame, interval = picked
        self.assertEqual((frame.width, frame.height), (1280, 720))
        self.assertAlmostEqual(self.uvc.fps(interval), 30.0, places=2)

    def test_high_speed_alts_carry_extra_transactions_per_microframe(self):
        # A full-speed device leaves bits 12:11 of wMaxPacketSize at zero, so
        # this is the half of that field the CODEC and mic could never test.
        alts = self.uvc.alt_settings(_C920E)
        self.assertEqual({a.transfer for a in alts}, {"isoc"})
        self.assertEqual({a.endpoint for a in alts}, {0x81})
        self.assertEqual(max(a.per_frame for a in alts), 3)
        self.assertEqual(min(a.per_frame for a in alts), 1)
        # Tiers must be usable as tiers: strictly increasing capacity.
        caps = [a.max_packet * a.per_frame for a in alts]
        self.assertEqual(caps, sorted(caps))

    def test_alt_selection_walks_the_tiers(self):
        alts = self.uvc.alt_settings(_C920E)
        self.assertEqual(self.uvc.alt_for_payload(alts, 128).max_packet, 192)
        big = self.uvc.alt_for_payload(alts, 3072)
        self.assertIsNotNone(big)
        self.assertEqual(big.max_packet * big.per_frame, 3072)
        self.assertIsNone(self.uvc.alt_for_payload(alts, 99999))

    def test_the_two_class_parsers_do_not_claim_each_other_s_devices(self):
        # This configuration is video-only -- the C920e presents its
        # microphone as a separate USB device, not another interface here.
        # So the pair is a real cross-check: the video parser must recognise
        # it and the audio parser must not, and both class-specific
        # descriptor namespaces are 0x24 with overlapping subtypes, which is
        # exactly how a parser ends up confidently reading the wrong device.
        self.assertTrue(self.uvc.has_video(_C920E))
        self.assertFalse(uac.has_audio(_C920E))
        self.assertEqual(uac.streams(_C920E), ())
        # And the reverse, against the audio device captured from this bench.
        self.assertFalse(self.uvc.has_video(_CODEC))
        self.assertEqual(self.uvc.formats(_CODEC), ())
        self.assertEqual(self.uvc.alt_settings(_CODEC), ())


if __name__ == "__main__":
    unittest.main()
