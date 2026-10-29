# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Talk to a USB serial device on the host port.

Opens the first CDC-ACM device on the host port, writes a short line, and
prints whatever comes back. A USB-serial adapter, another microcontroller's
CDC console, or a PyDevices board presenting CDC all work, because CDC is a
standard class with no bespoke protocol. The device side is ordinary CDC:
MicroPython's built-in console, or ``dev.functions("cdc")``. ``BAUDRATE`` is
sent to the device as its line coding, which a USB-serial adapter needs to
talk to whatever is on its far side. A microcontroller's own console ignores
it. Set it to ``None`` to send none.

Needs: a board with USB host support (firmware built with usbhost), such as
an ESP32-S3, and a CDC device on the host port.

Run it:

    mpftp run examples/usb_serial.py
"""

import time

import usb.core
from usb.host.cdc import Serial

# Change these to match your setup.
BAUDRATE = 115200


def find_cdc(timeout_ms=15000):
    deadline = time.ticks_add(time.ticks_ms(), timeout_ms)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        dev = usb.core.find(custom_match=Serial.match)
        if dev is not None:
            return dev
        time.sleep_ms(250)
    return None


def main(seconds=15):
    # Start the host for CDC only, then wait for a device to appear.
    usb.core.start(classes=("cdc",))
    dev = find_cdc()
    if dev is None:
        print("no CDC device found")
        usb.core.stop()
        return

    print("CDC device {:04x}:{:04x} {} {}".format(
        dev.idVendor, dev.idProduct, dev.manufacturer, dev.product))
    # A read waits up to 20 ms for its first byte, then takes what's there.
    port = Serial(dev, baudrate=BAUDRATE, timeout=20)
    msg = b"hello from the USB host\r\n"
    n = port.write(msg)
    print("wrote", n, "bytes:", msg)

    # Print everything the device sends until the time is up.
    t0 = time.ticks_ms()
    total = 0
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < seconds * 1000:
            got = port.read(256)
            if got:
                total += len(got)
                print("rx:", got)
    finally:
        print("total bytes read:", total)
        port.close()
        usb.core.stop()


if __name__ == "__main__":
    main()
