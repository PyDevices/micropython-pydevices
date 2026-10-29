# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Nudge the computer's pointer as a USB mouse, then put it back.

The board presents itself as a mouse. ``usb_hid`` gives CircuitPython's
``devices`` and Adafruit's ``adafruit_hid`` moves the pointer, so the same
code runs on CircuitPython. Success is the pointer moving 80 pixels right
and back, and the program printing that it did.

Needs: a board with USB device support and the ``usb-hid`` and
``adafruit_hid`` packages. Install them with ``mip`` if your firmware
doesn't freeze them:

    mip.install("github:PyDevices/pydevices/usb-hid")
    mip.install("github:PyDevices/pydevices/bledev_hid")

Run it:

    mpftp run examples/hid_mouse.py
"""

import time

import usb_hid
from adafruit_hid.mouse import Mouse

usb_hid.enable()
time.sleep(2)  # let the computer enumerate the mouse

# Move the pointer right in small steps, then back the same way.
mouse = Mouse(usb_hid.devices)
for _ in range(40):
    mouse.move(x=2)
    time.sleep_ms(10)
for _ in range(40):
    mouse.move(x=-2)
    time.sleep_ms(10)
print("moved 80 pixels right and back")
