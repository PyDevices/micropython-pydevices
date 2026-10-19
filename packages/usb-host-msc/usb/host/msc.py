# MicroPython USB mass storage host: a flash drive as a block device.
# MIT license; Copyright (c) 2026 Brad Barnett
#
#     import usb.core, vfs
#     from usb.host.msc import Drive
#
#     drive = Drive(usb.core.find(custom_match=Drive.match))
#     vfs.mount(vfs.VfsFat(drive), "/usb")       # a FAT-formatted drive
#     print(os.listdir("/usb"))
#
# Bulk-Only Transport and the few SCSI commands a filesystem needs (READ
# CAPACITY, READ(10), WRITE(10), TEST UNIT READY, REQUEST SENSE), in Python on
# usb.core's bulk transfers. Drive has readblocks, writeblocks and ioctl, so
# vfs can mount it like an SD card. FAT needs 512-byte blocks, which nearly
# every flash drive has.
import errno
import struct
import time

import usb.core

# ESP32 builds' errno has no EPIPE; the C module raises MicroPython's 32.
_EPIPE = getattr(errno, "EPIPE", 32)

_CBW = 0x43425355
_CSW = 0x53425355
_MAX_BLOCKS = 64    # per command: 32 KB at 512 bytes


class Drive:
    @staticmethod
    def match(dev):
        """For usb.core.find(custom_match=...): a Bulk-Only mass storage device."""
        return Drive._interface(dev) is not None

    @staticmethod
    def _interface(dev):
        cfg = dev.configuration_descriptor()
        i, itf, ep_in, ep_out = cfg[0], None, None, None
        while i + 2 <= len(cfg) and cfg[i] >= 2:
            if cfg[i + 1] == 4:
                if itf is not None and ep_in and ep_out:
                    break
                ok = cfg[i + 5] == 0x08 and cfg[i + 7] == 0x50 and cfg[i + 3] == 0
                itf, ep_in, ep_out = (cfg[i + 2] if ok else None), None, None
            elif cfg[i + 1] == 5 and itf is not None and cfg[i + 3] & 3 == 2:
                if cfg[i + 2] & 0x80:
                    ep_in = ep_in or cfg[i + 2]
                else:
                    ep_out = ep_out or cfg[i + 2]
            i += cfg[i]
        return (itf, ep_in, ep_out) if itf is not None and ep_in and ep_out else None

    def __init__(self, dev, lun=0, timeout=5000):
        found = self._interface(dev) if dev is not None else None
        if found is None:
            raise ValueError("not a Bulk-Only mass storage device")
        self.dev = dev
        self.lun = lun
        self.timeout = timeout
        self._itf, self._in, self._out = found
        self._tag = 0
        self._csw = bytearray(13)
        # A drive just plugged in answers NOT READY for a while.
        for _ in range(20):
            if self._command(bytes((0x00, 0, 0, 0, 0, 0))) == 0:   # TEST UNIT READY
                break
            self._sense()
            time.sleep_ms(100)
        cap = bytearray(8)
        if self._command(bytes((0x25,)) + bytes(9), data_in=cap) != 0:
            raise OSError(5, "READ CAPACITY failed")
        last, self.block_size = struct.unpack(">II", cap)
        self.blocks = last + 1

    # -- Bulk-Only Transport

    def _command(self, cb, data_in=None, data_out=None):
        """One command: the CBW, its data either way, then the CSW. Returns
        the CSW status (0 passed, 1 failed, 2 phase error)."""
        self._tag = (self._tag + 1) & 0xFFFFFFFF
        length = len(data_in) if data_in is not None else len(data_out) if data_out is not None else 0
        flags = 0x80 if data_in is not None else 0
        cbw = struct.pack("<IIIBBB", _CBW, self._tag, length, flags, self.lun, len(cb)) + cb + bytes(16 - len(cb))
        self.dev.write(self._out, cbw, self.timeout)
        try:
            if data_in is not None and length:
                # One transfer: it ends full, or early on a short packet.
                self.dev.read(self._in, data_in, self.timeout)
            elif data_out is not None and length:
                self.dev.write(self._out, data_out, self.timeout)
        except usb.core.USBError as e:
            if e.errno != _EPIPE:   # a stalled data phase; the CSW still follows
                raise
            ep = self._in if data_in is not None else self._out
            self.dev.ctrl_transfer(0x02, 0x01, 0, ep, None, 1000)   # CLEAR_FEATURE(ENDPOINT_HALT)
        try:
            self.dev.read(self._in, self._csw, self.timeout)
        except usb.core.USBError as e:
            if e.errno != _EPIPE:
                raise
            self.dev.ctrl_transfer(0x02, 0x01, 0, self._in, None, 1000)
            self.dev.read(self._in, self._csw, self.timeout)
        sig, tag, _, status = struct.unpack("<IIIB", self._csw)
        if sig != _CSW or tag != self._tag:
            self._reset()
            return 2
        if status == 2:
            self._reset()
        return status

    def _reset(self):
        # Bulk-Only Mass Storage Reset, then clear both halts (BOT 5.3.4).
        self.dev.ctrl_transfer(0x21, 0xFF, 0, self._itf, None, 1000)
        for ep in (self._in, self._out):
            self.dev.ctrl_transfer(0x02, 0x01, 0, ep, None, 1000)

    def _sense(self):
        sense = bytearray(18)
        self._command(bytes((0x03, 0, 0, 0, 18, 0)), data_in=sense)
        return sense[2] & 0x0F, sense[12], sense[13]   # key, ASC, ASCQ

    def _io(self, op, block_num, buf, write):
        count = len(buf) // self.block_size
        mv = memoryview(buf)
        done = 0
        while done < count:
            n = min(count - done, _MAX_BLOCKS)
            cb = struct.pack(">BBIBHB", op, 0, block_num + done, 0, n, 0)
            part = mv[done * self.block_size:(done + n) * self.block_size]
            status = self._command(cb, data_out=part) if write else self._command(cb, data_in=part)
            if status != 0:
                key, asc, ascq = self._sense()
                raise OSError(5, "%s block %d: sense %x/%02x/%02x" % (
                    "write" if write else "read", block_num + done, key, asc, ascq))
            done += n

    # -- block device

    def readblocks(self, block_num, buf, offset=0):
        if offset:
            raise OSError(22, "whole blocks only")
        self._io(0x28, block_num, buf, False)

    def writeblocks(self, block_num, buf, offset=0):
        if offset:
            raise OSError(22, "whole blocks only")
        self._io(0x2A, block_num, buf, True)

    def ioctl(self, op, arg):
        if op == 4:     # block count
            return self.blocks
        if op == 5:     # block size
            return self.block_size
        if op in (1, 2, 3, 6):  # init, deinit, sync, erase
            return 0
        return None
