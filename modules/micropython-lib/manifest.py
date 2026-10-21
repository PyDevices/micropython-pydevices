# The packages from micropython-lib that PyDevices' APIs lean on, frozen in so
# an OmniPython firmware never asks anyone to mip.install them first (on stock
# MicroPython you install them from micropython-lib's index, then ours):
#
#   aioble                       bledev's BLE, on boards
#   sdcard, bmi270               the SD cards and IMUs the board configs use
#   usb-device and its classes   usb.device, for the USB packages and midiports
#   __future__                   from __future__ import annotations
#
# They come from the micropython-lib checkout the MicroPython tree carries,
# whose files MicroPython's manifest tool resolves with require(). The device
# classes go in as a set: a frozen usb.device hides any usb/device/ in /lib (a
# package is found once, not merged across sys.path).
for _pkg in (
    "aioble",
    "sdcard",
    "bmi270",
    "__future__",
    "usb-device",
    "usb-device-cdc",
    "usb-device-hid",
    "usb-device-keyboard",
    "usb-device-midi",
    "usb-device-mouse",
):
    require(_pkg)
