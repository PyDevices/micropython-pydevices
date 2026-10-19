# The DEV-KIT's frozen content: the port's and the modules you named
# (modules/manifest.py), plus the USB packages this board's apps import and
# midiports, frozen so the board runs without a mip install. Only this variant
# freezes them; every other build still installs them with mip.
#
# usbif's packages/ is registered as a library, so their own require("usb-core")
# resolves there; usb-device-audio's require("usb-device") and usb-device-midi
# come from micropython-lib. Nothing here is frozen unless the build carries
# usbif, whose _usbif the host packages need.
import os

include("../../../../modules/manifest.py")

_names = [n.strip() for n in os.environ.get("PYDEVICES_MODULES", "").split(",")]
_usbif = os.path.realpath("../../../../modules/usbif")
if any(n in ("all", "usbif") or os.path.realpath(n) == _usbif for n in _names if n):
    add_library("usbif", _usbif + "/packages")
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
    ):
        require(_pkg, library="usbif")
    # A frozen usb.device hides any usb/device/ in /lib (a package is found
    # once, not merged across sys.path), so every device class goes in.
    for _pkg in ("usb-device-midi", "usb-device-cdc", "usb-device-hid"):
        require(_pkg)
    # midiports' board half (UART, USB device, USB host); the desktop ports stay
    # out. Its own manifest.py would call metadata() a second time in this
    # context, so the files are named here, as that manifest names them.
    package(
        "midiports",
        files=("__init__.py", "uart.py", "usb_device.py", "usb_host.py"),
        base_path="../../../../modules/pydevices/midiports",
    )
