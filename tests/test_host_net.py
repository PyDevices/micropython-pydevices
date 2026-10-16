# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""usb.host.net must find a phone's RNDIS adapter and do RNDIS's handshake.

The descriptor is an Android phone's with USB tethering on: RNDIS control
(class E0/01/03) with its interrupt endpoint, the CDC data interface with
its bulk pair, then adb. The fake device answers the way RNDIS does, by
queueing a reply for GET_ENCAPSULATED_RESPONSE.
"""

import pathlib
import struct
import sys
import types
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "packages" / "usb-host-net"))

if not hasattr(__import__("time"), "sleep_ms"):
    import time as _time
    _time.sleep_ms = lambda ms: None
    _time.ticks_ms = lambda: 0
    _time.ticks_add = lambda a, b: a + b
    _time.ticks_diff = lambda a, b: a - b

started = []
_usbif = types.ModuleType("_usbif")
_usbif.host_net_start = lambda *a: started.append(a)
_usbif.host_net_stop = lambda: started.append("stop")
_usbif.host_net_info = lambda: (1, ("192.168.42.7", "255.255.255.0", "192.168.42.129", "192.168.42.129"), (3, 4, 0, 0, 0))
_usbif.host_net_default = lambda: None
sys.modules["_usbif"] = _usbif
core = types.ModuleType("usb.core")
core.USBError = OSError
usb = types.ModuleType("usb")
usb.__path__ = [str(ROOT / "packages" / "usb-host-net" / "usb")]
usb.core = core
sys.modules["usb"] = usb
sys.modules["usb.core"] = core

from usb.host.net import Tether  # noqa: E402

S21 = bytes([
    9, 2, 0, 0, 3, 1, 0, 0x80, 250,
    8, 0x0B, 0, 2, 0xE0, 0x01, 0x03, 0,              # IAD
    9, 4, 0, 0, 1, 0xE0, 0x01, 0x03, 0,
    5, 0x24, 0, 0x10, 0x01,
    7, 5, 0x81, 3, 8, 0, 9,
    9, 4, 1, 0, 2, 0x0A, 0, 0, 0,
    7, 5, 0x8E, 2, 0, 2, 0,
    7, 5, 0x0F, 2, 0, 2, 0,
    9, 4, 2, 0, 2, 0xFF, 0x42, 0x01, 0,
    7, 5, 0x82, 2, 0, 2, 0,
    7, 5, 0x01, 2, 0, 2, 0,
])
MAC = bytes((0x02, 0x11, 0x22, 0x33, 0x44, 0x55))


class FakeRndis:
    _id = 7

    def __init__(self):
        self.sent = []
        self.reply = None

    def configuration_descriptor(self):
        return S21

    def ctrl_transfer(self, rt, req, value, index, data, timeout=None):
        if rt == 0x21:
            assert req == 0 and index == 0
            self.sent.append(bytes(data))
            mtype, mlen, rid = struct.unpack_from("<III", data)
            assert mlen == len(data)
            done = mtype | 0x80000000
            if mtype == 2:
                self.reply = struct.pack("<IIIIIIIIIIIII", done, 52, rid, 0, 1, 0, 1, 0, 1, 0x4000, 3, 0, 0)
            elif mtype == 4:
                self.reply = struct.pack("<IIIIII", done, 24 + 6, rid, 0, 6, 16) + MAC
            else:
                self.reply = struct.pack("<IIII", done, 16, rid, 0)
            return len(data)
        assert rt == 0xA1 and req == 1
        r, self.reply = self.reply, None
        if r is None:
            return 0
        data[:len(r)] = r
        return len(r)


class TetherTests(unittest.TestCase):
    def test_finds_the_phone(self):
        self.assertTrue(Tether.match(FakeRndis()))

    def test_handshake_then_start(self):
        started.clear()
        dev = FakeRndis()
        net = Tether(dev)
        self.assertTrue(net.active(True))
        types_sent = [struct.unpack_from("<I", m)[0] for m in dev.sent]
        self.assertEqual(types_sent, [2, 4, 5])
        oid, length = struct.unpack_from("<II", dev.sent[2], 12)
        self.assertEqual((oid, length), (0x0001010E, 4))
        self.assertEqual(net.config("mac"), MAC)
        self.assertEqual(started, [(7, 1, 0x8E, 0x0F, 512, MAC, True)])
        self.assertTrue(net.isconnected())
        self.assertEqual(net.ifconfig()[2], "192.168.42.129")
        net.active(False)
        self.assertEqual(started[-1], "stop")


if __name__ == "__main__":
    unittest.main()
