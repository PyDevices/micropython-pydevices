# MicroPython USB CDC host: a USB serial device on the host port.
# MIT license; Copyright (c) 2026 Brad Barnett
#
#     import usb.core
#     from usb.host.cdc import Serial
#
#     port = Serial(usb.core.find(custom_match=Serial.match))
#     port.write(b"print(1 + 1)\r\n")      # another MicroPython board's REPL
#     print(port.read(64))
#
# CDC-ACM, the class a USB serial adapter or a microcontroller's own USB
# console uses. Serial finds the data interface's bulk pair, raises DTR and
# RTS (a MicroPython board's console stays silent until DTR says a terminal
# is attached), and reads and writes the way machine.UART does. Line coding
# (baud rate and framing) is sent when given, for a real adapter; a
# microcontroller's console ignores it.
import struct

import usb.core

_SET_LINE_CODING = 0x20
_SET_CONTROL_LINE_STATE = 0x22


def _find(dev):
    """(control interface, data interface, bulk IN, bulk OUT, IN packet), or None."""
    cfg = dev.configuration_descriptor()
    i, comm, data, cur, ep_in, ep_out, mps = cfg[0], None, None, None, None, None, 64
    while i + 2 <= len(cfg) and cfg[i] >= 2:
        if cfg[i + 1] == 4:
            cur = cfg[i + 2]
            if cfg[i + 5] == 0x02 and comm is None:
                comm = cur
            if cfg[i + 5] == 0x0A and data is None:
                data = cur
        elif cfg[i + 1] == 5 and cur is not None and cur == data and cfg[i + 3] & 3 == 2:
            if cfg[i + 2] & 0x80:
                ep_in, mps = cfg[i + 2], cfg[i + 4] | cfg[i + 5] << 8
            else:
                ep_out = cfg[i + 2]
        i += cfg[i]
    if data is None or ep_in is None or ep_out is None:
        return None
    return (comm if comm is not None else data), data, ep_in, ep_out, mps


class Serial:
    @staticmethod
    def match(dev):
        """For usb.core.find(custom_match=...): a CDC-ACM device."""
        return _find(dev) is not None

    def __init__(self, dev, *, baudrate=None, timeout=0, dtr=True):
        """``timeout`` in ms: how long a read waits for the first byte (0: a
        poll that returns at once)."""
        found = _find(dev) if dev is not None else None
        if found is None:
            raise ValueError("not a CDC-ACM device")
        self.dev = dev
        self._comm, self._data, self._in, self._out, mps = found
        self.timeout = timeout
        self._buf = bytearray(mps)
        self._pending = memoryview(self._buf)[:0]
        if baudrate:
            self.init(baudrate)
        self.dtr(dtr)

    def init(self, baudrate=115200, bits=8, parity=None, stop=1):
        stop_code = {1: 0, 1.5: 1, 2: 2}[stop]
        parity_code = {None: 0, 1: 1, 0: 2}.get(parity, 0)
        self.dev.ctrl_transfer(0x21, _SET_LINE_CODING, 0, self._comm,
                               struct.pack("<IBBB", baudrate, stop_code, parity_code, bits), 1000)

    def dtr(self, on=True, rts=True):
        self.dev.ctrl_transfer(0x21, _SET_CONTROL_LINE_STATE, (1 if on else 0) | (2 if rts else 0),
                               self._comm, None, 1000)

    def _fill(self, timeout):
        if self._pending:
            return True
        try:
            n = self.dev.read(self._in, self._buf, max(1, timeout))
        except usb.core.USBTimeoutError:
            return False
        self._pending = memoryview(self._buf)[:n]
        return n > 0

    def any(self):
        """Bytes ready to read without waiting (what's arrived, at least)."""
        self._fill(1)
        return len(self._pending)

    def readinto(self, buf, nbytes=None):
        want = len(buf) if nbytes is None else min(nbytes, len(buf))
        got = 0
        timeout = self.timeout
        while got < want and self._fill(timeout):
            take = min(want - got, len(self._pending))
            buf[got:got + take] = self._pending[:take]
            self._pending = self._pending[take:]
            got += take
            timeout = 1     # after the first bytes, take only what's already here
        return got or None

    def read(self, nbytes=None):
        buf = bytearray(nbytes or 256)
        n = self.readinto(buf)
        return bytes(buf[:n]) if n else None

    def write(self, buf):
        return self.dev.write(self._out, buf, 1000)

    def close(self):
        self.dev._release_all()

    deinit = close
