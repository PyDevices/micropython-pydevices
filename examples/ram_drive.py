"""A RAM disk the computer sees as a removable drive.

``usb-device-msc`` serves any MicroPython block device; here it's a block of
RAM, formatted FAT on the board with one file on it, so the drive opens with
something to read. Contrast with ``sd_drive.py``, which serves a real card.

    mpftp run -d COM49 examples/ram_drive.py

Install the package first on a firmware that doesn't freeze it:
``mip.install("github:PyDevices/usbif/packages/usb-device-msc")``.

**One writer.** While the computer has the drive, don't mount the same block
device on the board: two filesystems caching one disk corrupt it. Eject it on
the computer first, then mount it here to read what was written.

**Console.** The board's own serial port stays up beside the drive
(``builtin_driver=True``), so the REPL survives on the same cable.
"""
import os

import usb.device
from usb.device.msc import MSCInterface, RAMBlockDev

BLOCKS = 128  # 64 KiB

bdev = RAMBlockDev(BLOCKS)
os.VfsFat.mkfs(bdev)
os.mount(os.VfsFat(bdev), "/ram")
with open("/ram/hello.txt", "w") as f:
    f.write("Written on the board. Copy files on and off like any drive.\n")
os.umount("/ram")

msc = MSCInterface(bdev, product="RAM drive")
usb.device.get().init(msc, builtin_driver=True)
print("RAM drive: {} KiB on USB".format(BLOCKS // 2))
