# The DEV-KIT's frozen content: the port's and the modules you named
# (modules/manifest.py), plus the USB packages this board's apps import and
# midiports, frozen so the board runs without a mip install. Only this variant
# freezes them; every other build still installs them with mip.
#
# They come from omnipython-lib, whose usb-* packages require each other there;
# usb-device-audio's require("usb-device") and the device classes come from
# micropython-lib (modules/micropython-lib freezes every class; a frozen
# usb.device hides any usb/device/ in /lib). Nothing here is frozen unless the
# build carries usbhost, whose _usbhost the host packages need.
import os

include("../../../../modules/manifest.py")

_names = [n.strip() for n in os.environ.get("PYDEVICES_MODULES", "").split(",")]
_usbhost = os.path.realpath("../../../../modules/usbhost")
if any(n in ("all", "usbhost") or os.path.realpath(n) == _usbhost for n in _names if n):
    include("../../../../modules/library.py")
    for _pkg in (
        "usb-core",
        "usb-host-audio",
        "usb-host-video",
        "usb-host-net",
        "usb-host-hid",
        "usb-host-msc",
        "usb-host-cdc",
        "usb-device-audio",
        "usb-device-video",
        "usb-device-msc",
        # midiports' board half (UART, USB device, USB host); the desktop ports
        # stay out. adafruit_midi isn't in it: a firmware that wants it frozen
        # adds Adafruit's files itself.
        "midiports",
    ):
        require(_pkg, library="omnipython-lib")
    # The device classes the USB packages and midiports import, if the build
    # didn't already freeze micropython-lib's (modules/micropython-lib does).
    for _pkg in ("usb-device-midi", "usb-device-cdc", "usb-device-hid"):
        require(_pkg)
