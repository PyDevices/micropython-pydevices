# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""List the USB devices plugged into the board's host port.

The shortest program that proves usbhost works: it starts the host, waits
a moment for devices to enumerate, and prints the vendor and product ID of
each one. `host_enum.py` shows how to read their names and watch them come
and go.

Needs: a board with USB host support (firmware built with usbhost), the
`usb-core` package (`mip.install("github:PyDevices/pydevices/usb-core")`),
and a device in the host port. An ESP32-S3 needs a powered hub or OTG
adapter, because some boards don't switch VBUS.

Run it:

    mpremote run usbhost_simpletest.py

Tested on: Arduino GIGA R1 WiFi with the GIGA Display Shield, MicroPython
v1.30.0-preview.160.gb70ae1aca6, with a PNY USB flash drive in the USB-A
port: it printed `1 USB device(s) found` and `154b:009f`. Waveshare
ESP32-P4-WIFI6-DEV-KIT, the same MicroPython, with nothing plugged in: it
printed `0 USB device(s) found` and stopped cleanly.
"""

import time

import usb.core

# Start the host and give attached devices a moment to enumerate.
usb.core.start()
try:
    time.sleep_ms(1500)

    # Print each device's vendor and product ID.
    devices = list(usb.core.find(find_all=True))
    print("%d USB device(s) found" % len(devices))
    for device in devices:
        print("%04x:%04x" % (device.idVendor, device.idProduct))
finally:
    # Release the port for the next program.
    usb.core.stop()
