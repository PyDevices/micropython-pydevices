"""Read a file off a hosted USB drive, check its hash, and time it.

    mpftp run -d COM49 examples/usb_drive_read.py

The board hosts a USB drive (a flash stick, a card reader, or another board
running ``sd_drive.py``), mounts its first FAT partition read-only, lists the
top folder and reads one file end to end. It prints the file's SHA-256 and
the read rate, so you can tell the bytes arrived whole: compare the hash with
one taken on the PC, or set ``EXPECT_SHA256`` below and the script compares
for you.

``FILE`` picks the file; leave it ``None`` and the largest file in the top
folder is read. Nothing is written: the partition is mounted read-only, so
the drive is exactly as it was afterwards.

**Pairing.** ``sd_drive.py`` on an ESP32-P4 panel serves its microSD card as
the drive; this script on an S3 hosts it. ``usb_drive_mount.py`` is the
shorter version that only lists, and ``usb_drive_log.py`` the one that writes.

**FAT only.** MicroPython's FAT driver is built without exFAT (upstream's
default, and ours), so a drive formatted exFAT, which is what a PC picks for
cards over 32 GB, won't mount. A board that formats a large card itself with
``vfs.VfsFat.mkfs()`` makes it FAT32.

**Rate.** Each SCSI command carries up to 64 blocks, and its round trip is
most of what the rate measures; a larger ``CHUNK`` can mean fewer of them.
"""
import hashlib
import os
import time

import usb.core
import vfs
from usb.host.msc import Drive

FILE = None             # a name in the top folder, or None for the largest
EXPECT_SHA256 = None    # hex string to compare against, or None
CHUNK = 4096            # bytes per read(); a multiple of the block size
MOUNT = "/usb"


def find_drive(timeout_ms=15000):
    deadline = time.ticks_add(time.ticks_ms(), timeout_ms)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        dev = usb.core.find(custom_match=Drive.match)
        if dev is not None:
            return dev
        time.sleep_ms(250)
    return None


def first_partition(drive):
    """``(start, count)`` of the filesystem on the drive, or None.

    Usually that is the first FAT partition in the drive's MBR. A drive with
    no partition table, which is how MicroPython's own ``mkfs`` formats a
    small one, has its FAT boot sector at block 0, and the whole drive is
    the filesystem. MicroPython's FAT driver finds either by itself when
    handed the whole drive; this is here to say which it is, and to stop
    early on a drive that has neither.
    """
    mbr = bytearray(drive.block_size)
    drive.readblocks(0, mbr)
    if mbr[510] != 0x55 or mbr[511] != 0xAA:
        return None
    if mbr[0] in (0xEB, 0xE9) and (b"FAT" in mbr[54:62] or b"FAT" in mbr[82:90]):
        return 0, drive.blocks
    for i in range(4):
        e = 446 + i * 16
        if mbr[e + 4] in (0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E):
            start = int.from_bytes(mbr[e + 8:e + 12], "little")
            count = int.from_bytes(mbr[e + 12:e + 16], "little")
            return start, count
    return None


def mount(drive):
    """Mount the drive's filesystem read-only at ``MOUNT``. The whole drive
    goes to ``VfsFat``, partitioned or not: it reads the MBR itself."""
    vfs.mount(vfs.VfsFat(drive), MOUNT, readonly=True)


def listing(path):
    files = []
    for name in os.listdir(path):
        st = os.stat(path + "/" + name)
        is_dir = st[0] & 0x4000
        print("  {} {:>10}  {}".format("dir " if is_dir else "file", st[6], name))
        if not is_dir:
            files.append((st[6], name))
    return files


def read_and_hash(path):
    h = hashlib.sha256()
    buf = bytearray(CHUNK)
    total = 0
    t0 = time.ticks_ms()
    with open(path, "rb") as f:
        while True:
            n = f.readinto(buf)
            if not n:
                break
            h.update(memoryview(buf)[:n])
            total += n
    ms = max(1, time.ticks_diff(time.ticks_ms(), t0))
    return total, ms, h.digest()


def main():
    usb.core.start(classes=("msc",))
    try:
        dev = find_drive()
        if dev is None:
            print("no USB drive attached")
            return
        print("drive {:04x}:{:04x} {} {}".format(
            dev.idVendor, dev.idProduct, dev.manufacturer, dev.product))
        drive = Drive(dev)
        print("{} blocks of {} bytes ({} MB)".format(
            drive.blocks, drive.block_size, drive.blocks * drive.block_size // (1024 * 1024)))

        part = first_partition(drive)
        if part is None:
            print("no FAT filesystem found: neither an MBR partition nor a boot sector at block 0")
            return
        if part[0]:
            print("FAT partition at block {}, {} blocks".format(*part))
        else:
            print("no partition table: FAT from block 0")
        mount(drive)
        try:
            print("{}:".format(MOUNT))
            files = listing(MOUNT)
            name = FILE or (max(files)[1] if files else None)
            if name is None:
                print("no files to read")
                return
            size, ms, digest = read_and_hash(MOUNT + "/" + name)
            hexdigest = "".join("{:02x}".format(b) for b in digest)
            print("read {}: {} bytes in {} ms, {} KB/s".format(
                name, size, ms, size * 1000 // ms // 1024))
            print("sha256", hexdigest)
            if EXPECT_SHA256:
                print("hash matches" if hexdigest == EXPECT_SHA256.lower()
                      else "HASH MISMATCH, expected " + EXPECT_SHA256)
        finally:
            vfs.umount(MOUNT)
    finally:
        usb.core.stop()


if __name__ == "__main__":
    main()
