"""The board is a USB mouse: nudge the computer's pointer, then put it back.

CircuitPython's mouse code, unchanged: ``usb_hid`` gives CircuitPython's
``devices`` and Adafruit's ``adafruit_hid`` moves the pointer.

    mpftp run -d COM49 examples/hid_mouse.py

Install the packages first on a firmware that doesn't freeze them:
``mip.install("github:PyDevices/pydevices/usb-hid")`` and
``mip.install("github:PyDevices/pydevices/bledev_hid")`` for ``adafruit_hid``.
"""
import time

import usb_hid
from adafruit_hid.mouse import Mouse

usb_hid.enable()
time.sleep(2)  # let the computer enumerate the mouse

mouse = Mouse(usb_hid.devices)
for _ in range(40):
    mouse.move(x=2)
    time.sleep_ms(10)
for _ in range(40):
    mouse.move(x=-2)
    time.sleep_ms(10)
print("moved 80 pixels right and back")
