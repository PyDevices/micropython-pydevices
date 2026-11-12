# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""List a phone's photos and save one to the board.

The board is the USB host and the phone its device. ``usb.host.mtp`` speaks the
protocol a phone uses to share files with a computer (MTP), so no app is
needed on the phone. The program lists the pictures in the phone's camera
folder and copies the first to the board's filesystem. Nothing is written to
the phone.

Needs: a board with USB host support (firmware built with usbhost), the
``usb-host-mtp`` package, and an Android phone in the host port. Unlock the
phone and choose File transfer in its USB notification. It asks "Allow access
to phone data?" each time it is plugged in: tap Allow, and run the program
again if it said no storage was shared.

Run it:

    mpftp run --follow examples/usb_host_mtp_simpletest.py
    mpftp get /photo.jpg photo.jpg

Success is the photos listed and a JPEG on the board.

Tested on: Waveshare ESP32-P4-WIFI6-DEV-KIT on MicroPython
v1.30.0-preview.160.gb70ae1aca6 (built 2026-11-11), behind its onboard hub, hosting a Samsung Galaxy S21 in File transfer mode, with
``usb-host-mtp`` copied onto the board: it listed a photo in the camera
folder and saved a byte-identical copy.
"""

import time

import usb.core
from usb.host.mtp import MTP

# Change these to match your setup.
FILENAME = "/photo.jpg"

# Start the host before the phone is plugged in, then wait for it.
usb.core.start()
try:
    device = None
    while device is None:
        time.sleep_ms(250)
        device = usb.core.find(custom_match=MTP.match)
    phone = MTP(device)
    print("Phone:", phone.device_info()[:2])

    # List the camera folder's pictures, and copy the first to a file.
    photos = phone.photos()
    for handle in photos:
        info = phone.info(handle)
        print(info.name, info.size, "bytes")
    if photos:
        size = phone.get(photos[0], FILENAME)
        print("Saved", size, "bytes to", FILENAME)
    phone.close()
finally:
    # Release the port for the next program.
    usb.core.stop()
