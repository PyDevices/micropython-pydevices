# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""usb.core's polling read allocates nothing, on MicroPython itself.

    MICROPYPATH=packages/usb-core micropython tests/unix_core_poll.py

A loop that polls an interrupt endpoint every frame can't afford garbage.
Device.read(ep, buf, timeout=..., raise_on_timeout=False) must answer 0 on
"nothing yet", and a report when one came, with the heap locked: any
allocation on that path raises MemoryError here. A fake _usbif stands in
for the C module (its host_xfer allocates nothing either). The control:
the raising read must fail under the lock, or the lock isn't proving
anything.
"""
import sys

import micropython

_report = [0]


class _FakeHost:
    def host_start(self, classes, full_speed=False):
        pass

    def host_stop(self):
        pass

    def host_devices(self):
        return [(7, 0x046D, 0xC52B, 0, 0, 0, "full")]

    def host_desc(self, dev_id):
        # A configuration with one interface and an interrupt IN endpoint.
        return bytes((9, 2, 25, 0, 1, 1, 0, 0x80, 50,
                      9, 4, 0, 0, 1, 3, 1, 1, 0,
                      7, 5, 0x81, 3, 8, 0, 10))

    def host_claim(self, dev_id, itf, alt=0):
        return False        # already ours: no CLEAR_FEATURE

    def host_ctrl(self, dev_id, setup, buf, timeout):
        return 0

    def host_xfer(self, dev_id, ep, buf, timeout, raise_on_timeout=True):
        if _report[0]:
            buf[0] = _report[0]
            return 1
        if raise_on_timeout:
            raise OSError(110)
        return 0


sys.modules["_usbif"] = _FakeHost()

import usb.core  # noqa: E402

dev = usb.core.find()
buf = bytearray(8)
dev.read(0x81, buf, timeout=1, raise_on_timeout=False)   # the first read caches the endpoint's interface


def polls(dev, buf, times):
    # In a function: a module-level loop and its globals allocate by
    # themselves, which would hide what this is checking.
    empty = 0
    i = 0
    while i < times:
        if dev.read(0x81, buf, timeout=1, raise_on_timeout=False) == 0:
            empty += 1
        i += 1
    return empty


n = empty = 0
micropython.heap_lock()
try:
    empty = polls(dev, buf, 1000)
    _report[0] = 7
    n = dev.read(0x81, buf, timeout=1, raise_on_timeout=False)
finally:
    micropython.heap_unlock()
assert empty == 1000, empty
assert n == 1 and buf[0] == 7, (n, buf[0])

# The control: the raising read allocates its exception, so it must fail
# under the same lock.
_report[0] = 0
failed = False
micropython.heap_lock()
try:
    dev.read(0x81, buf, timeout=1)
except MemoryError:
    failed = True
except usb.core.USBTimeoutError:
    pass
finally:
    micropython.heap_unlock()
assert failed, "the raising read didn't allocate under heap_lock: the check proves nothing"

print("unix core poll: PASS (1000 empty polls and a report, no allocation)")
