# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""examples/hid_host.py turns usb.host.hid's key events into PyDevices ones.

``usb.host.hid`` returns ``(KEY_DOWN, keycode, modifiers)`` and the like, with
adafruit_hid's Keycode values. The example maps each to an ``events.Key``,
modifier keys as keys of their own, and lights Caps Lock when it is pressed.

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
KEY_DOWN, KEY_UP, CONSUMER_DOWN, MOUSE_EVENT = 1, 2, 3, 5
LSHIFT = 0xE1


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
    """usb.host.hid.Device's surface: events from a list, LEDs recorded."""

    events = []
    leds = []

    def __init__(self, dev, protocol=1, boot=True):
        self.dev = dev
        self._events = list(self.events)

    @staticmethod
    def match(protocol=1):
        return lambda dev: True

    def read(self, timeout=1):
        return self._events.pop(0) if self._events else None

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
    hid.KEY_DOWN, hid.KEY_UP = KEY_DOWN, KEY_UP
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


class HidHostEvents(unittest.TestCase):
    def setUp(self):
        self.keys = _load_example().KeyboardEvents()

    def _feed(self, *evs):
        return [e for e in (self.keys.feed(ev) for ev in evs) if e is not None]

    def test_a_key_down_and_up(self):
        got = self._feed((KEY_DOWN, H, 0), (KEY_UP, H, 0))
        self.assertEqual([(e.type, e.scancode, e.mod) for e in got],
                         [(events.KEYDOWN, H, 0), (events.KEYUP, H, 0)])

    def test_modifier_is_its_own_key_around_the_one_it_modifies(self):
        got = self._feed((KEY_DOWN, LSHIFT, 2), (KEY_DOWN, H, 2), (KEY_UP, H, 0), (KEY_UP, LSHIFT, 0))
        self.assertEqual([(e.type, e.scancode) for e in got],
                         [(events.KEYDOWN, LSHIFT), (events.KEYDOWN, H), (events.KEYUP, H), (events.KEYUP, LSHIFT)])
        self.assertNotEqual(got[1].mod, 0, "shift is held while h is down")
        self.assertEqual(got[2].mod, 0)

    def test_mouse_and_media_events_are_not_keys(self):
        self.assertEqual(self._feed((MOUSE_EVENT, 1, 5, -5, 0), (CONSUMER_DOWN, 0xE9, 0)), [])

    def test_a_usage_with_no_key_is_unknown(self):
        (e,) = self._feed((KEY_DOWN, 0xA5, 0))
        self.assertEqual((e.name, e.key, e.scancode), ("Unknown", None, 0xA5))


class HidHostMain(unittest.TestCase):
    def test_reads_a_keyboard_and_lights_caps_lock(self):
        example = _load_example()
        _HidDevice.events = [(KEY_DOWN, H, 0), (KEY_UP, H, 0), (KEY_DOWN, CAPS, 0), (KEY_UP, CAPS, 0)]
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
