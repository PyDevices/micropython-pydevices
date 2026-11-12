# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""List the files on a USB drive plugged into the board.

The board is the USB host and the drive its device: a flash stick, a card
reader, or another board running ``usb_device_msc_simpletest.py``.
``usb.host.msc.Drive`` is a MicroPython block device, so MicroPython's FAT
driver mounts it like any other. The program mounts the drive read-only,
prints the files in its top folder with their sizes, and unmounts it. Nothing
is written to the drive. ``usb_drive_read.py`` reads a file and times it, and
``usb_drive_log.py`` writes.

Needs: a board with USB host support (firmware built with usbhost), the
``usb-host-msc`` package, and a FAT drive in the host port. The firmware's FAT
driver has no exFAT, so a drive a computer formatted exFAT won't mount. Behind
a hub on an ESP32-P4, leave ``FULL_SPEED = True``: a high-speed hub can't
reach a full-speed drive otherwise.

Run it:

    mpftp run --follow examples/usb_host_msc_simpletest.py

Success is the drive's size and its files listed.

Tested on: Waveshare ESP32-P4-WIFI6-DEV-KIT on MicroPython
v1.30.0-preview.160.gb70ae1aca6 (built 2026-11-11), behind its onboard hub, with FULL_SPEED = True, hosting an Arduino GIGA R1 WiFi serving a 128 KiB RAM disk
(``usb.device.msc``): the drive's size and its two files were listed.
"""

import os
import time

import usb.core
import vfs
from usb.host.msc import Drive

# Change these to match your setup.
FULL_SPEED = True
MOUNT = "/usb"

# Start the host and wait for a drive to enumerate.
usb.core.start(full_speed=FULL_SPEED, classes=("msc",))
try:
    device = None
    while device is None:
        time.sleep_ms(250)
        device = usb.core.find(custom_match=Drive.match)
    drive = Drive(device)
    print("Drive:", drive.blocks * drive.block_size // 1024, "KiB")

    # Mount it read-only, list the top folder, and let it go.
    vfs.mount(vfs.VfsFat(drive), MOUNT, readonly=True)
    try:
        for name, kind, _, size in os.ilistdir(MOUNT):
            print(name, "(folder)" if kind == 0x4000 else "%d bytes" % size)
    finally:
        vfs.umount(MOUNT)
finally:
    # Release the port for the next program.
    usb.core.stop()
