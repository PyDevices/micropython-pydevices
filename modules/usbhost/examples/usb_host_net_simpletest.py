# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Get the board online through an Android phone's USB tethering.

The board is the USB host and the phone its device. With USB tethering on, the
phone offers an RNDIS network adapter and gives the board an address by DHCP.
``usb.host.net.Tether`` is the board's end of that link. The program prints the
address it was given. ``phone_tether.py`` goes on to fetch a web page over it.

Needs: a board with USB host support (firmware built with usbhost), the
``usb-host-net`` package, and an Android phone in the host port. On the phone,
turn on Settings > Connections > Mobile Hotspot and Tethering > USB tethering.
Its switch stays grey until the phone is plugged in.

Run it:

    mpftp run --follow examples/usb_host_net_simpletest.py

Success is an address from the phone, such as 192.168.42.x.

Tested on: Waveshare ESP32-P4-WIFI6-DEV-KIT on MicroPython
v1.30.0-preview.160.gb70ae1aca6 (built 2026-11-11), behind its onboard hub, hosting a Samsung Galaxy S21 with USB tethering on: the
phone gave the board 10.20.186.148.
"""

import time

import usb.core
from usb.host.net import Tether

# Change these to match your setup.
WAIT_PHONE_MS = 120000  # a phone can take a while to change its USB mode

# Start the host and wait for the phone's network adapter.
usb.core.start()
try:
    print("Waiting for the phone: turn on USB tethering")
    link = Tether(wait_ms=WAIT_PHONE_MS)
    link.active(True)

    # The phone gives an address by DHCP once the link is up.
    while not link.isconnected():
        time.sleep_ms(100)
    print("Address from the phone:", link.ifconfig())
    link.active(False)
finally:
    # Release the port for the next program.
    usb.core.stop()
