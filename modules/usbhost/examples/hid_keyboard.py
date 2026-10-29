# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Type a short string into the computer as a USB keyboard.

The board presents itself as a keyboard. ``usb_hid`` (our package over
micropython-lib's ``usb-device-hid``) gives the same ``devices`` that
CircuitPython's does, so Adafruit's ``adafruit_hid`` code runs unchanged.
Any operating system accepts it with no driver. After typing, the program
prints the Caps Lock state the computer last set, read from the keyboard's
LED report. Success is the greeting appearing in whatever text box has focus.

Needs: a board with USB device support and the ``usb-hid`` and
``adafruit_hid`` packages. Install them with ``mip`` if your firmware
doesn't freeze them:

    mip.install("github:PyDevices/pydevices/usb-hid")
    mip.install("github:PyDevices/pydevices/bledev_hid")

Run it, with a text box focused on the computer:

    mpftp run examples/hid_keyboard.py

The board's serial port stays up beside the keyboard, so the REPL keeps
working over the same cable.
"""

import time

import usb_hid
from adafruit_hid.keyboard import Keyboard
from adafruit_hid.keyboard_layout_us import KeyboardLayoutUS

usb_hid.enable()
time.sleep(2)  # let the computer enumerate the keyboard

kb = Keyboard(usb_hid.devices)
KeyboardLayoutUS(kb).write("Hello from a MicroPython keyboard!\n")

print("Caps Lock is", "on" if kb.led_on(Keyboard.LED_CAPS_LOCK) else "off")
