"""Talk to a USB serial device the board is hosting.

Opens the first CDC-ACM device on the host port, writes a short line, and
prints whatever comes back. A USB-serial adapter, another MCU's CDC console,
or a PyDevices board presenting CDC all work -- standard class, no bespoke
protocol.

    mpftp run -d COM49 examples/usb_serial.py

**Pairing.** Device side is ordinary CDC (MicroPython's built-in console, or
``dev.functions("cdc")``). Host side is this script on an S3.

``BAUDRATE`` is sent to the device as its line coding, which a USB-serial
adapter needs to talk to whatever is on its far side; a microcontroller's own
console ignores it. Set it to ``None`` to send none.
"""

import time

import usb.core
from usb.host.cdc import Serial

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
