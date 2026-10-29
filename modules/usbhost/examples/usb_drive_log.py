# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Append readings to a USB flash drive on the host port and read them back.

A datalogger case: the board writes what it produced onto a stick, then
carries on. The program mounts the stick's FAT filesystem read-write, appends
to a log file, unmounts to flush, and remounts read-only to read the lines
back. That proves the bytes reached the medium, not just a cache.

``usb.host.msc.Drive`` writes with SCSI WRITE(10). Whether a mount may write
is the mount's choice. The other drive examples pass ``readonly=True``,
because the safe answer to "may I write to the drive somebody just plugged in"
is no. This is the one that says yes. As on the device side, a filesystem
with two writers is a corrupted one, so nothing else should hold the stick
while the board has it.

Needs: a board with USB host support (firmware built with usbhost) and a FAT
USB flash drive on the host port.

Run it:

    mpftp run examples/usb_drive_log.py

Tested on: an ESP32-P4-WIFI6-DEV-KIT on MicroPython
v1.30.0-preview.160.gb70ae1aca6 (built 2026-10-21),
hosting a PNY 8 GB FAT flash drive behind its onboard hub.
"""

import os
import time

import usb.core
import vfs
from usb.host.msc import Drive

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
else:
    print("drive: {:04x}:{:04x} {} {}".format(
        dev.idVendor, dev.idProduct, dev.manufacturer, dev.product))
    drive = Drive(dev)

    # The whole drive goes to VfsFat: on a partitioned stick it finds the
    # first FAT partition itself.
    vfs.mount(vfs.VfsFat(drive), "/usb")        # read-write this time
    print("mounted /usb read-write")
    print("before:", os.listdir("/usb"))

    # Append, the way a logger would: open in append mode so re-running
    # accumulates rather than replacing.
    with open("/usb/SENSOR.LOG", "a") as f:
        for i in range(5):
            f.write("t={} reading={}\n".format(time.ticks_ms(), 20 + i))
    print("appended 5 lines")

    vfs.umount("/usb")
    print("unmounted (flushed)")

    # Remount and read it back -- proving the bytes reached the medium and
    # not just a cache.
    vfs.mount(vfs.VfsFat(drive), "/usb", readonly=True)
    print("after:", os.listdir("/usb"))
    with open("/usb/SENSOR.LOG") as f:
        data = f.read()
    print("--- SENSOR.LOG ({} bytes) ---".format(len(data)))
    print(data)
    vfs.umount("/usb")

    usb.core.stop()
    print("done")
