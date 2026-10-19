# usb.host.cdc on usb.core: type a line into a hosted MicroPython board's REPL
# over its USB serial port and read the answer back.
import time

import usb.core
from usb.host.cdc import Serial


def run():
    usb.core.start(classes=("cdc",))
    dev = None
    for _ in range(40):
        dev = usb.core.find(custom_match=Serial.match)
        if dev:
            break
        time.sleep_ms(250)
    port = Serial(dev, timeout=200)
    while port.read(256):
        pass
    t0 = time.ticks_ms()
    port.write(b"print('cdc', 6 * 7)\r\n")
    got = b""
    end = time.ticks_add(t0, 2000)
    while time.ticks_diff(end, time.ticks_ms()) > 0 and b"cdc 42" not in got:
        got += port.read(256) or b""
    ms = time.ticks_diff(time.ticks_ms(), t0)
    port.close()
    return {"device": "%s %s" % (dev.manufacturer, dev.product), "answered": b"cdc 42" in got, "ms": ms,
            "got": got[-60:]}
