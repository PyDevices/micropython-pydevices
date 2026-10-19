# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""examples/hid_host.py must read a keyboard that prefixes a report ID.

A keyboard sharing its interface with a mouse (a board running
``hid_keyboard.py``) sends ``[report id, modifiers, 0, key, ...]``. Fed to the
boot-report decoder as it came, the report ID 1 read as the Left Ctrl bit, so
every key arrived with Ctrl held and Ctrl was never released.

The example runs on fakes of ``usb.core`` and ``usb.host.hid``, installed
only while it loads: other test modules put their own ``usb`` packages in
``sys.modules``, and this one leaves theirs as it found them.
"""

import contextlib
import importlib.util
import io
import pathlib
import sys
import types
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

import _env  # noqa: E402, F401

import events  # noqa: E402

H = 0x0B
CAPS = 0x39


class _Clock(types.ModuleType):
    """MicroPython's ticks on a clock that moves 10 ms per look."""

    def __init__(self):
        super().__init__("time")
        self.now = 0

    def ticks_ms(self):
        self.now += 10
        return self.now

    def ticks_add(self, a, b):
        return a + b

    def ticks_diff(self, a, b):
        return a - b

    def sleep_ms(self, ms):
        self.now += ms


class _USBError(OSError):
    pass


class _UsbDevice:
    idVendor, idProduct = 0x303A, 0x4001
    manufacturer, product = "Maker", "Keyboard"


class _HidDevice:
    """usb.host.hid.Device's surface: reports from a list, LEDs recorded."""

    reports = []
    leds = []

    def __init__(self, dev, protocol=1, boot=True):
        self.dev = dev
        self._reports = list(self.reports)

    @staticmethod
    def match(protocol=1):
        return lambda dev: True

    def read(self, timeout=1):
        return memoryview(bytearray(self._reports.pop(0))) if self._reports else None

    def set_leds(self, leds):
        _HidDevice.leds.append(leds)

    def close(self):
        pass


def _fakes():
    usb = types.ModuleType("usb")
    usb.__path__ = []
    core = types.ModuleType("usb.core")
    core.USBError = _USBError
    core.start = lambda full_speed=False, classes=None: None
    core.stop = lambda: None
    core.find = lambda find_all=False, **kw: _UsbDevice()
    host = types.ModuleType("usb.host")
    host.__path__ = []
    hid = types.ModuleType("usb.host.hid")
    hid.KEYBOARD, hid.MOUSE = 1, 2
    hid.Device = _HidDevice
    usb.core, usb.host, host.hid = core, host, hid
    return {"usb": usb, "usb.core": core, "usb.host": host, "usb.host.hid": hid}


def _load_example():
    clean = {k: v for k, v in sys.modules.items() if k != "usb" and not k.startswith("usb.")}
    with mock.patch.dict(sys.modules, clean, clear=True):
        sys.modules.update(_fakes())
        spec = importlib.util.spec_from_file_location(
            "hid_host_example", ROOT / "examples" / "hid_host.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    module.time = _Clock()
    return module


class HidHostReports(unittest.TestCase):
    def setUp(self):
        self.example = _load_example()

    def _keys(self, raw_reports):
        decoder = self.example.KeyboardDecoder()
        out = []
        for raw in raw_reports:
            buf = bytearray(64)
            buf[:len(raw)] = raw
            report = self.example.keyboard_report(buf, len(raw))
            if report is not None:
                out.extend((e.type, e.mod) for e in decoder.feed(report))
        return out

    def test_report_id_is_stripped(self):
        got = self._keys([bytes([1, 0, 0, H, 0, 0, 0, 0, 0]), bytes([1] + [0] * 8)])
        self.assertEqual([t for t, _ in got], [events.KEYDOWN, events.KEYUP])
        self.assertTrue(all(mod == 0 for _, mod in got), got)

    def test_plain_boot_report_still_reads(self):
        got = self._keys([bytes([0, 0, H, 0, 0, 0, 0, 0]), bytes(8)])
        self.assertEqual([t for t, _ in got], [events.KEYDOWN, events.KEYUP])

    def test_mouse_report_on_the_same_interface_is_skipped(self):
        self.assertEqual(self._keys([bytes([2, 1, 5, 0xFB, 0, 0])]), [])

    def test_modifier_is_its_own_key_around_the_one_it_modifies(self):
        decoder = self.example.KeyboardDecoder()
        got = [(e.type, e.scancode) for r in (bytes([2, 0, H, 0, 0, 0, 0, 0]), bytes(8))
               for e in decoder.feed(r)]
        self.assertEqual(got, [(events.KEYDOWN, H), (events.KEYDOWN, 0xE1),
                               (events.KEYUP, 0xE1), (events.KEYUP, H)])

    def test_rollover_leaves_the_held_keys_alone(self):
        decoder = self.example.KeyboardDecoder()
        decoder.feed(bytes([0, 0, H, 0, 0, 0, 0, 0]))
        self.assertEqual(decoder.feed(bytes([0, 0] + [1] * 6)), ())
        self.assertEqual([e.type for e in decoder.feed(bytes(8))], [events.KEYUP])


class HidHostMain(unittest.TestCase):
    def test_reads_a_report_id_keyboard_and_lights_caps_lock(self):
        example = _load_example()
        _HidDevice.reports = [bytes([1, 0, 0, H, 0, 0, 0, 0, 0]), bytes([1] + [0] * 8),
                              bytes([1, 0, 0, CAPS, 0, 0, 0, 0, 0]), bytes([1] + [0] * 8)]
        _HidDevice.leds = []
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            example.main(seconds=1)
        lines = out.getvalue().splitlines()
        self.assertIn("keyboard 303a:4001 Maker Keyboard", lines[0])
        self.assertEqual([ln.split()[0] for ln in lines[1:-1]], ["DOWN", "UP", "DOWN", "UP"])
        self.assertTrue(all("mod=0x00" in ln for ln in lines[1:-1]), lines)
        self.assertEqual(_HidDevice.leds, [0x02])
        self.assertEqual(lines[-1], "done")


if __name__ == "__main__":
    unittest.main()
