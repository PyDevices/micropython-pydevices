# MicroPython manifest for usbif.
#
# usbif's native half, the `_usbif` module built from ``src/``: the USB host
# port and the transfers and streams the USB packages build on. The packages
# themselves (``packages/``: usb-core, usb-host-*, usb-device-*, usb-hid) are
# not frozen; install the ones a board needs with mip, so they upgrade without
# a reflash.

# MicroPython 1.29: the manifest names its own C module.
c_module(".")  # this directory holds the micropython.cmake / micropython.mk for the C half
