# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Read the start of a board's flash through its DFU bootloader.

The board is the USB host and the other board its device, waiting in its DFU
bootloader. ``usb.host.dfu`` reads the flash a block at a time. The program
reads 4 KB from the start of the application and prints its SHA-256 and the
first bytes, so you can compare them with the firmware file the other board
was flashed with. It only reads: nothing is downloaded or erased.

Needs: a board with USB host support (firmware built with usbhost), the
``usb-host-dfu`` package, and a board in its DFU bootloader in the host port.
An Arduino GIGA R1 enters it when you double-tap its reset button. Behind a
hub on an ESP32-P4, leave ``FULL_SPEED = True``: the bootloader is a
full-speed device, and the P4's high-speed port can't reach one through a hub.

Run it:

    mpftp run --follow examples/usb_host_dfu_simpletest.py

Success is the bootloader's memory regions listed and the flash's hash. The
board stays in its bootloader afterwards: reset it to run its program again.

Tested on: Waveshare ESP32-P4-WIFI6-DEV-KIT on MicroPython
v1.30.0-preview.160.gb70ae1aca6 (built 2026-11-11), behind its onboard hub, with
``usb-host-dfu`` copied onto the board, reading an Arduino GIGA R1 WiFi: the
hash matched the first 4 KB of the firmware file the GIGA was flashed from.
"""

import hashlib
import time

import usb.core
from usb.host.dfu import Dfu

# Change these to match your setup.
FULL_SPEED = True
ADDRESS = 0x08000000  # where to start reading; the GIGA's bootloader starts at its firmware
LENGTH = 4096  # how many bytes to read

# Start the host and wait for the bootloader to enumerate.
usb.core.start(full_speed=FULL_SPEED)
try:
    time.sleep_ms(3000)
    bootloader = Dfu(usb.core.find(custom_match=Dfu.match))
    print("Memory:", bootloader.alternates())

    # Read the flash a block at a time and hash it.
    digest = hashlib.sha256()
    first = b""
    for block in bootloader.chunks(ADDRESS, LENGTH):
        digest.update(block)
        first = first or block[:16]
    print("First bytes:", first.hex())
    print("SHA-256:", digest.digest().hex())
finally:
    # Release the port for the next program.
    usb.core.stop()
