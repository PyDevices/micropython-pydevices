# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""examples/usb_drive_read.py finds the filesystem on a partitioned drive
and on one with no partition table, and mounts either.

MicroPython's ``vfs.VfsFat.mkfs()`` formats a small drive with no MBR, its
FAT boot sector at block 0, and a large one with an MBR and one FAT32
partition. The example has to mount either, and refuse a drive that is
neither.

The example runs on fakes of ``usb.core``, ``usb.host.msc`` and ``vfs``,
installed only while it loads: other test modules put their own ``usb``
packages in ``sys.modules``, and this one leaves theirs as it found them.
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


class _Drive:
    """usb.host.msc.Drive's block-device surface, block 0 given."""

    def __init__(self, block0, blocks):
        self.block0 = block0
        self.blocks = blocks
        self.block_size = 512

    def readblocks(self, block_num, buf, offset=0):
        assert block_num == 0
        buf[:] = self.block0


class _Vfs(types.ModuleType):
    """vfs.mount and VfsFat, recording what was mounted."""

    def __init__(self):
        super().__init__("vfs")
        self.mounts = []
        self.unmounts = []

    def VfsFat(self, bdev):
        return ("VfsFat", bdev)

    def mount(self, fs, path, readonly=False):
        self.mounts.append((fs, path, readonly))

    def umount(self, path):
        self.unmounts.append(path)


def _fakes(vfs):
    usb = types.ModuleType("usb")
    usb.__path__ = []
    core = types.ModuleType("usb.core")
    host = types.ModuleType("usb.host")
    host.__path__ = []
    msc = types.ModuleType("usb.host.msc")
    msc.Drive = _Drive
    usb.core, usb.host, host.msc = core, host, msc
    return {"usb": usb, "usb.core": core, "usb.host": host, "usb.host.msc": msc, "vfs": vfs}


def _load_example():
    vfs = _Vfs()
    clean = {k: v for k, v in sys.modules.items() if k != "usb" and not k.startswith("usb.")}
    with mock.patch.dict(sys.modules, clean, clear=True):
        sys.modules.update(_fakes(vfs))
        spec = importlib.util.spec_from_file_location(
            "usb_drive_read_example", ROOT / "examples" / "usb_drive_read.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    return module, vfs


def _mbr(ptype, start, count):
    b = bytearray(512)
    e = 446
    b[e + 4] = ptype
    b[e + 8:e + 12] = start.to_bytes(4, "little")
    b[e + 12:e + 16] = count.to_bytes(4, "little")
    b[510:512] = b"\x55\xaa"
    return b


def _boot_sector(label_at, label):
    b = bytearray(512)
    b[0:3] = b"\xeb\x3c\x90"
    b[label_at:label_at + 8] = label
    b[510:512] = b"\x55\xaa"
    return b


class FirstPartition(unittest.TestCase):
    def setUp(self):
        self.example, self.vfs = _load_example()

    def test_mbr_fat32_partition(self):
        drive = _Drive(_mbr(0x0C, 8192, 62676992), 62685184)
        self.assertEqual(self.example.first_partition(drive), (8192, 62676992))

    def test_no_partition_table_fat16(self):
        drive = _Drive(_boot_sector(54, b"FAT16   "), 4096)
        self.assertEqual(self.example.first_partition(drive), (0, 4096))

    def test_no_partition_table_fat32(self):
        drive = _Drive(_boot_sector(82, b"FAT32   "), 4096)
        self.assertEqual(self.example.first_partition(drive), (0, 4096))

    def test_blank_drive(self):
        self.assertIsNone(self.example.first_partition(_Drive(bytearray(512), 4096)))

    def test_non_fat_partition(self):
        drive = _Drive(_mbr(0x83, 2048, 100000), 102048)   # a Linux partition
        self.assertIsNone(self.example.first_partition(drive))


class Mount(unittest.TestCase):
    """The whole drive goes to VfsFat, read-only, MBR or not: the FAT
    driver finds the partition itself, so a partition's offset never gets
    applied twice."""

    def _run_main(self, block0, blocks):
        example, vfs = _load_example()
        drive = _Drive(block0, blocks)
        dev = types.SimpleNamespace(idVendor=0x0781, idProduct=0x5567,
                                    manufacturer="SanDisk", product="Cruzer")
        example.usb.core.start = lambda **kw: None
        example.usb.core.stop = lambda: None
        example.find_drive = lambda: dev
        example.Drive = lambda d: drive
        example.listing = lambda path: []
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            example.main()
        return drive, vfs, out.getvalue()

    def test_partitioned_drive_mounts_whole(self):
        drive, vfs, out = self._run_main(_mbr(0x0C, 8192, 62676992), 62685184)
        self.assertEqual(vfs.mounts, [(("VfsFat", drive), "/usb", True)])
        self.assertEqual(vfs.unmounts, ["/usb"])
        self.assertIn("FAT partition at block 8192", out)

    def test_drive_with_no_mbr_mounts_whole(self):
        drive, vfs, out = self._run_main(_boot_sector(82, b"FAT32   "), 4096)
        self.assertEqual(vfs.mounts, [(("VfsFat", drive), "/usb", True)])
        self.assertIn("no partition table", out)

    def test_drive_with_neither_is_refused(self):
        _, vfs, out = self._run_main(bytearray(512), 4096)
        self.assertEqual(vfs.mounts, [])
        self.assertIn("no FAT filesystem found", out)


if __name__ == "__main__":
    unittest.main()
