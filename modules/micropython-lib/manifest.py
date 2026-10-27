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

# The frozen usb package also hides every other usb/ in /lib, so our USB
# packages, which install there, can't be added with mip on this firmware.
# They go in with it: the device classes always (they need only usb.device),
# and usb.core with the host classes when the build carries usbhost, whose
# _usbhost they run on.
import os

include("../library.py")
for _pkg in ("usb-device-audio", "usb-device-video", "usb-device-msc"):
    require(_pkg, library="omnipython-lib")
_names = [n.strip() for n in os.environ.get("PYDEVICES_MODULES", "").split(",")]
_usbhost = os.path.realpath("../usbhost")
if any(n in ("all", "usbhost") or os.path.realpath(n) == _usbhost for n in _names if n):
    for _pkg in (
        "usb-core",
        "usb-host-audio",
        "usb-host-video",
        "usb-host-net",
        "usb-host-hid",
        "usb-host-msc",
        "usb-host-cdc",
    ):
        require(_pkg, library="omnipython-lib")
