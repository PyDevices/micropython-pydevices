"""The board is a USB keyboard: type a short string into the computer.

This is CircuitPython's keyboard code, unchanged: ``usb_hid`` (our package
over micropython-lib's ``usb-device-hid``) gives the same ``devices`` that
CircuitPython's does, and Adafruit's ``adafruit_hid`` drives them. Any
operating system's keyboard stack accepts it, with no driver to install.

    mpftp run -d COM49 examples/hid_keyboard.py

Install the packages first on a firmware that doesn't freeze them:
``mip.install("github:PyDevices/usbif/packages/usb-hid")`` and
``mip.install("github:PyDevices/pydevices/bledev_hid")`` for ``adafruit_hid``.

Click into a text box before it types. **Console**: the board's serial port
stays up beside the keyboard, so the REPL survives on the same cable.

**LEDs.** After typing it prints the lock-key state the computer last set
(Caps, Num, Scroll), from the keyboard's LED report, the same call a control
surface would read.
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
