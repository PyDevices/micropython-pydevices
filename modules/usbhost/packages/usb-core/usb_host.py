# MicroPython usb_host: start this board's USB host port, as CircuitPython's
# usb_host does, for usb.core to find devices on.
# MIT license; Copyright (c) 2026 Brad Barnett
#
#     import usb_host, usb.core
#
#     port = usb_host.Port()                  # the native USB port, as host
#     port = usb_host.Port(full_speed=True)   # ESP32-P4: full-speed devices behind a hub
#     for dev in usb.core.find(find_all=True):
#         print(dev.product)
#     port.deinit()
#
# On CircuitPython, Port(dp, dm) names the pins of a PIO USB port. Here the
# host is the board's own USB controller, so there are no pins to name.
import usb.core


class Port:
    def __init__(self, dp=None, dm=None, *, full_speed=False, classes=None):
        if dp is not None or dm is not None:
            raise ValueError("the host is this board's native USB port; it takes no pins")
        usb.core.start(full_speed=full_speed, classes=classes)

    def deinit(self):
        usb.core.stop()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.deinit()
