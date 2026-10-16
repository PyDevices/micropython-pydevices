# MicroPython USB mass storage module
# MIT license; Copyright (c) 2026 Brad Barnett
#
# A USB mass storage device (Bulk-Only Transport, SCSI transparent command
# set) over any MicroPython block device, in the style of micropython-lib's
# usb-device-* packages:
#
#     import usb.device
#     from usb.device.msc import MSCInterface
#
#     msc = MSCInterface(bdev)                  # an SD card, a partition, RAM
#     usb.device.get().init(msc, builtin_driver=True)
#
# The computer then sees a drive. bdev needs readblocks(n, buf),
# writeblocks(n, buf) and ioctl(4) (the block count); ioctl(5) gives the block
# size when it isn't 512. Pass readonly=True to refuse writes.
#
# Don't mount the same block device on the board while the computer has it:
# two filesystems caching one disk will corrupt it.
from micropython import const
import struct

from .core import Interface, split_bmRequestType

_INTERFACE_CLASS_MSC = const(0x08)
_INTERFACE_SUBCLASS_SCSI = const(0x06)
_PROTOCOL_BBB = const(0x50)

_EP_IN_FLAG = const(1 << 7)

_REQ_TYPE_CLASS = const(0x01)
_REQ_GET_MAX_LUN = const(0xFE)
_REQ_BOT_RESET = const(0xFF)

_CBW_SIGNATURE = const(0x43425355)
_CSW_SIGNATURE = const(0x53425355)
_CBW_LEN = const(31)
_CSW_LEN = const(13)

_STATUS_PASSED = const(0)
_STATUS_FAILED = const(1)
_STATUS_PHASE_ERROR = const(2)

# SCSI operation codes.
_TEST_UNIT_READY = const(0x00)
_REQUEST_SENSE = const(0x03)
_INQUIRY = const(0x12)
_MODE_SENSE_6 = const(0x1A)
_START_STOP_UNIT = const(0x1B)
_PREVENT_ALLOW = const(0x1E)
_READ_FORMAT_CAPACITIES = const(0x23)
_READ_CAPACITY_10 = const(0x25)
_READ_10 = const(0x28)
_WRITE_10 = const(0x2A)
_VERIFY_10 = const(0x2F)
_SYNCHRONIZE_CACHE_10 = const(0x35)
_MODE_SENSE_10 = const(0x5A)

# Sense keys and additional sense codes.
_SENSE_NONE = const(0x00)
_SENSE_NOT_READY = const(0x02)
_SENSE_ILLEGAL_REQUEST = const(0x05)
_SENSE_DATA_PROTECT = const(0x07)
_ASC_INVALID_COMMAND = const(0x20)
_ASC_LBA_OUT_OF_RANGE = const(0x21)
_ASC_WRITE_PROTECTED = const(0x27)
_ASC_MEDIUM_NOT_PRESENT = const(0x3A)

_IDLE = const(0)
_DATA_IN = const(1)
_DATA_OUT = const(2)
_STATUS = const(3)

# Blocks moved per bulk transfer: 8 x 512 = 4 KB, one buffer made once.
_CHUNK_BLOCKS = const(8)


class MSCInterface(Interface):
    """A USB drive backed by ``bdev``. ``bulk_len`` is the bulk endpoints'
    packet size: 64 at full speed, 512 at high speed."""

    def __init__(
        self,
        bdev,
        *,
        readonly=False,
        vendor="PyDevice",
        product="USB drive",
        revision="1.0",
        bulk_len=64,
        interface_str=None,
    ):
        super().__init__()
        self.bdev = bdev
        self.readonly = readonly
        self.bulk_len = bulk_len
        self.interface_str = interface_str
        self.block_size = bdev.ioctl(5, 0) or 512
        self._inquiry = bytearray(36)
        self._inquiry[1] = 0x80  # removable
        self._inquiry[2] = 0x04  # SPC-2
        self._inquiry[3] = 0x02  # response data format
        self._inquiry[4] = 31  # additional length
        for start, size, text in ((8, 8, vendor), (16, 16, product), (32, 4, revision)):
            raw = text.encode()[:size]
            self._inquiry[start : start + size] = raw + b" " * (size - len(raw))
        self._cbw = bytearray(_CBW_LEN)
        self._csw = bytearray(_CSW_LEN)
        self._reply = bytearray(36)
        self._buf = bytearray(self.block_size * _CHUNK_BLOCKS)
        self._mv = memoryview(self._buf)
        self._sense = (_SENSE_NONE, 0)
        self._state = _IDLE
        self.ep_in = self.ep_out = None
        #: Counts, for a test or a status display.
        self.blocks_read = 0
        self.blocks_written = 0
        self.commands = 0
        self.errors = 0
        #: True once the computer ejected the drive ("safely remove"); the
        #: medium then reads as absent until :meth:`insert`.
        self.ejected = False

    # -- descriptors

    def desc_cfg(self, desc, itf_num, ep_num, strs):
        if self.interface_str:
            strs.append(self.interface_str)
            istr = len(strs) - 1
        else:
            istr = 0
        desc.interface(
            itf_num, 2, _INTERFACE_CLASS_MSC, _INTERFACE_SUBCLASS_SCSI, _PROTOCOL_BBB, istr
        )
        self.ep_out = ep_num
        self.ep_in = ep_num | _EP_IN_FLAG
        desc.endpoint(self.ep_out, "bulk", self.bulk_len, 0)
        desc.endpoint(self.ep_in, "bulk", self.bulk_len, 0)

    def num_eps(self):
        return 1  # one OUT and one IN, the same number

    # -- lifecycle

    def on_open(self):
        super().on_open()
        self._state = _IDLE
        self._arm_cbw()

    def on_reset(self):
        super().on_reset()
        self._state = _IDLE

    def on_interface_control_xfer(self, stage, request):
        bmRequestType, bRequest, _, _, _ = request
        _, req_type, _ = split_bmRequestType(bmRequestType)
        if req_type != _REQ_TYPE_CLASS:
            return False
        if bRequest == _REQ_GET_MAX_LUN:
            return b"\x00"  # one logical unit
        if bRequest == _REQ_BOT_RESET:
            if stage == 3:  # the ACK stage: start over on a fresh CBW
                self._state = _IDLE
                self._arm_cbw()
            return True
        return False

    # -- the Bulk-Only state machine

    def _arm_cbw(self):
        if self.is_open() and not self.xfer_pending(self.ep_out):
            self.submit_xfer(self.ep_out, self._cbw, self._cbw_done)

    def _cbw_done(self, ep, result, n):
        if result != 0:
            return  # the device is going away; on_open re-arms
        if n != _CBW_LEN or struct.unpack_from("<I", self._cbw, 0)[0] != _CBW_SIGNATURE:
            # Not a valid CBW: per BOT 6.6.1, wait for a reset. Stalling both
            # endpoints is what a host expects; re-arming keeps us reachable
            # if it doesn't.
            self.errors += 1
            self._arm_cbw()
            return
        self.commands += 1
        tag, self._dlen, flags = struct.unpack_from("<IIB", self._cbw, 4)
        self._tag = tag
        self._dir_in = bool(flags & 0x80)
        self._done = 0
        cb = memoryview(self._cbw)[15:31]
        try:
            self._dispatch(cb)
        except Exception:
            self.errors += 1
            self._fail(_SENSE_ILLEGAL_REQUEST, _ASC_INVALID_COMMAND)

    def _dispatch(self, cb):
        op = cb[0]
        if op == _START_STOP_UNIT:
            if cb[4] & 0x02 and not cb[4] & 0x01:  # LoEj and not Start: eject
                self.ejected = True
            self._ok()
        elif self.ejected and op not in (_INQUIRY, _REQUEST_SENSE):
            self._fail(_SENSE_NOT_READY, _ASC_MEDIUM_NOT_PRESENT)
        elif op == _TEST_UNIT_READY or op in (_PREVENT_ALLOW, _SYNCHRONIZE_CACHE_10, _VERIFY_10):
            self._ok()
        elif op == _INQUIRY:
            self._send(self._inquiry, 36)
        elif op == _REQUEST_SENSE:
            r = self._reply
            for i in range(18):
                r[i] = 0
            r[0] = 0x70
            r[2] = self._sense[0]
            r[7] = 10
            r[12] = self._sense[1]
            self._sense = (_SENSE_NONE, 0)
            self._send(r, 18)
        elif op == _READ_CAPACITY_10:
            struct.pack_into(">II", self._reply, 0, self._blocks() - 1, self.block_size)
            self._send(self._reply, 8)
        elif op == _READ_FORMAT_CAPACITIES:
            r = self._reply
            struct.pack_into(">IIBBH", r, 0, 8, self._blocks(), 0x02, 0, 0)
            struct.pack_into(">I", r, 8, self.block_size)
            r[8] = 0x02  # formatted media
            self._send(r, 12)
        elif op == _MODE_SENSE_6:
            r = self._reply
            r[0], r[1], r[2], r[3] = 3, 0, 0x80 if self.readonly else 0, 0
            self._send(r, 4)
        elif op == _MODE_SENSE_10:
            r = self._reply
            for i in range(8):
                r[i] = 0
            r[1] = 6
            r[3] = 0x80 if self.readonly else 0
            self._send(r, 8)
        elif op == _READ_10:
            lba, count = struct.unpack_from(">IxH", cb, 2)
            if lba + count > self._blocks():
                self._fail(_SENSE_ILLEGAL_REQUEST, _ASC_LBA_OUT_OF_RANGE)
                return
            self._lba, self._left = lba, count
            self._state = _DATA_IN
            self._read_chunk()
        elif op == _WRITE_10:
            lba, count = struct.unpack_from(">IxH", cb, 2)
            if self.readonly:
                self._fail(_SENSE_DATA_PROTECT, _ASC_WRITE_PROTECTED)
                return
            if lba + count > self._blocks():
                self._fail(_SENSE_ILLEGAL_REQUEST, _ASC_LBA_OUT_OF_RANGE)
                return
            self._lba, self._left = lba, count
            self._state = _DATA_OUT
            self._write_chunk_arm()
        else:
            self._fail(_SENSE_ILLEGAL_REQUEST, _ASC_INVALID_COMMAND)

    def insert(self):
        """Present the medium again after an eject."""
        self.ejected = False

    def _blocks(self):
        return self.bdev.ioctl(4, 0)

    # -- data phases

    def _send(self, data, n):
        """Answer a command with ``n`` bytes (cut to what the host asked for)."""
        n = min(n, self._dlen)
        if not n:
            self._ok()
            return
        self._state = _DATA_IN
        self._left = 0
        self.submit_xfer(self.ep_in, memoryview(data)[:n], self._in_done)

    def _read_chunk(self):
        k = min(self._left, _CHUNK_BLOCKS)
        mv = self._mv[: k * self.block_size]
        self.bdev.readblocks(self._lba, mv)
        self._lba += k
        self._left -= k
        self.blocks_read += k
        self.submit_xfer(self.ep_in, mv, self._in_done)

    def _in_done(self, ep, result, n):
        if result != 0:
            return
        self._done += n
        if self._left:
            self._read_chunk()
        else:
            self._status(_STATUS_PASSED)

    def _write_chunk_arm(self):
        k = min(self._left, _CHUNK_BLOCKS)
        self._chunk = k
        self.submit_xfer(self.ep_out, self._mv[: k * self.block_size], self._out_done)

    def _out_done(self, ep, result, n):
        if result != 0:
            return
        k = self._chunk
        self._done += n
        self.bdev.writeblocks(self._lba, self._mv[: k * self.block_size])
        self._lba += k
        self._left -= k
        self.blocks_written += k
        if self._left:
            self._write_chunk_arm()
        else:
            self._status(_STATUS_PASSED)

    # -- status

    def _ok(self):
        self._status(_STATUS_PASSED)

    def _fail(self, key, asc):
        self._sense = (key, asc)
        self.errors += 1
        if self._dlen and not self._dir_in:
            # The host is about to send data we won't use: take it and drop it.
            self._discard_left = self._dlen
            self._discard()
            return
        if self._dlen and self._dir_in:
            # The host waits for data: give it zeros (a short transfer would
            # need a stall the host must clear), then the failed status, after
            # which it asks REQUEST SENSE why.
            n = min(self._dlen, len(self._buf))
            for i in range(n):
                self._buf[i] = 0
            self._state = _DATA_IN
            self._left = 0
            self._fail_pending = True
            self.submit_xfer(self.ep_in, self._mv[:n], self._fail_in_done)
            return
        self._status(_STATUS_FAILED)

    def _fail_in_done(self, ep, result, n):
        if result == 0:
            self._done += n
            self._status(_STATUS_FAILED)

    def _discard(self):
        n = min(self._discard_left, len(self._buf))
        self.submit_xfer(self.ep_out, self._mv[:n], self._discard_done)

    def _discard_done(self, ep, result, n):
        if result != 0:
            return
        self._discard_left -= n
        self._done += n
        if self._discard_left > 0 and n:
            self._discard()
        else:
            self._status(_STATUS_FAILED)

    def _status(self, status):
        residue = self._dlen - self._done if self._dlen > self._done else 0
        struct.pack_into("<IIIB", self._csw, 0, _CSW_SIGNATURE, self._tag, residue, status)
        self._state = _STATUS
        self.submit_xfer(self.ep_in, self._csw, self._csw_done)

    def _csw_done(self, ep, result, n):
        self._state = _IDLE
        if result == 0:
            self._arm_cbw()


class RAMBlockDev:
    """A block device in RAM: ``RAMBlockDev(blocks)`` of 512 bytes each."""

    def __init__(self, blocks, block_size=512):
        self.block_size = block_size
        self.data = bytearray(blocks * block_size)

    def readblocks(self, n, buf, offset=0):
        start = n * self.block_size + offset
        buf[:] = self.data[start : start + len(buf)]

    def writeblocks(self, n, buf, offset=0):
        start = n * self.block_size + offset
        self.data[start : start + len(buf)] = buf

    def ioctl(self, op, arg):
        if op == 4:
            return len(self.data) // self.block_size
        if op == 5:
            return self.block_size
        if op == 6:  # erase
            return 0
        return None
