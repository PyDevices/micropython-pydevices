# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Mount a USB flash drive on the host port and list its files.

``usb.host.msc.Drive`` sits between the stick and MicroPython's FAT driver,
with ``readblocks``, ``writeblocks`` and ``ioctl``, so mounting a stick is one
line:

    vfs.mount(vfs.VfsFat(Drive(dev)), "/usb")

Hand ``VfsFat`` the whole drive, not a partition. On a partitioned stick the
filesystem doesn't start at block 0, so the FAT driver reads the partition
table and finds the first FAT partition itself. The program prints the
partition table first so you can see what it found. It mounts read-only on
purpose, so listing a stick somebody just plugged in leaves it as it was;
``usb_drive_log.py`` is the one that writes.

Needs: a board with USB host support (firmware built with usbhost) and a FAT
USB flash drive on the host port.

Run it:

    mpftp run examples/usb_drive_mount.py

Tested on: an ESP32-P4-WIFI6-DEV-KIT on MicroPython
v1.30.0-preview.160.gb70ae1aca6 (built 2026-10-21),
hosting a PNY 8 GB FAT flash drive behind its onboard hub.
"""

import os
import time

import usb.core
import vfs
from usb.host.msc import Drive


def main():
    usb.core.start(classes=("msc",))

    dev = None
    for _ in range(20):
        dev = usb.core.find(custom_match=Drive.match)
        if dev is not None:
            break
        time.sleep_ms(500)
    if dev is None:
        print("no drive attached")
        usb.core.stop()
        return

    print("device {:04x}:{:04x} {} {}".format(
        dev.idVendor, dev.idProduct, dev.manufacturer, dev.product))
    drive = Drive(dev)
    bs = drive.block_size
    print("drive: {} MB in {}-byte blocks".format(drive.blocks * bs // (1024 * 1024), bs))

    mbr = bytearray(bs)
    drive.readblocks(0, mbr)
    if mbr[510] != 0x55 or mbr[511] != 0xAA:
        print("no MBR signature; not a partitioned disk")
    elif mbr[0] in (0xEB, 0xE9) and (b"FAT" in mbr[54:62] or b"FAT" in mbr[82:90]):
        print("no partition table: FAT from block 0")
    else:
        # Partition table: four 16-byte entries starting at 0x1BE. Type byte
        # at +4, first LBA at +8, block count at +12, both little-endian.
        for i in range(4):
            e = 446 + i * 16
            ptype = mbr[e + 4]
            if ptype == 0:
                continue
            start = int.from_bytes(mbr[e + 8:e + 12], "little")
            count = int.from_bytes(mbr[e + 12:e + 16], "little")
            print("partition {}: type 0x{:02x} start {} count {} ({} MB)".format(
                i + 1, ptype, start, count, count * bs // (1024 * 1024)))

    try:
        vfs.mount(vfs.VfsFat(drive), "/usb", readonly=True)
    except OSError as exc:
        print("mount failed:", exc)
        usb.core.stop()
        return

    print("mounted at /usb")
    try:
        for name in os.listdir("/usb"):
            try:
                st = os.stat("/usb/" + name)
                kind = "dir " if st[0] & 0x4000 else "file"
                print("  {} {:>9}  {}".format(kind, st[6], name))
            except OSError:
                print("  ?           ", name)
        # Read one text file back, to prove real file content crosses.
        for name in os.listdir("/usb"):
            if name.lower().endswith(".txt"):
                with open("/usb/" + name) as f:
                    data = f.read(200)
                print("--- {} ---".format(name))
                print(repr(data))
                break
    finally:
        vfs.umount("/usb")
        print("unmounted")

    usb.core.stop()
    print("done")


main()
