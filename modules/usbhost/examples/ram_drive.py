# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Show the computer a RAM disk as a removable drive.

``usb-device-msc`` serves any MicroPython block device. Here the device is
a block of RAM, formatted FAT on the board with one file on it, so the drive
opens with something to read. ``sd_drive.py`` serves a real card the same
way. Success is a 64 KiB drive appearing on the computer with ``hello.txt``
on it.

Only one side may write at a time. While the computer has the drive, don't
mount the same block device on the board, because two filesystems caching one
disk corrupt it. Eject the drive on the computer first, then mount it on the
board to read what was written.

Needs: a board with USB device support and the ``usb-device-msc`` package.
Install it with ``mip`` if your firmware doesn't freeze it:

    mip.install("github:PyDevices/pydevices/usb-device-msc")

Run it:

    mpftp run examples/ram_drive.py

The board's serial port stays up beside the drive (``builtin_driver=True``),
so the REPL keeps working over the same cable.
"""

import os

import usb.device
from usb.device.msc import MSCInterface, RAMBlockDev

# Change these to match your setup.
BLOCKS = 128  # 64 KiB

# Format the RAM block device as FAT and put one file on it.
bdev = RAMBlockDev(BLOCKS)
os.VfsFat.mkfs(bdev)
os.mount(os.VfsFat(bdev), "/ram")
with open("/ram/hello.txt", "w") as f:
    f.write("Written on the board. Copy files on and off like any drive.\n")
os.umount("/ram")

# Unmount it from the board, then hand it to the computer.
msc = MSCInterface(bdev, product="RAM drive")
usb.device.get().init(msc, builtin_driver=True)
print("RAM drive: {} KiB on USB".format(BLOCKS // 2))
