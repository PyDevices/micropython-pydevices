# Read a hosted keyboard's reports through usb.host.hid for a while.
import time

import usb.core
from usb.host.hid import Device, KEYBOARD, _interfaces


def run(seconds=6, vid=0x303A):
    usb.core.start()
    dev = None
    for _ in range(20):
        dev = usb.core.find(idVendor=vid, custom_match=lambda d: bool(_interfaces(d)))
        if dev:
            break
        time.sleep_ms(250)
    kbd = Device(dev, KEYBOARD)
    reports = []
    end = time.ticks_add(time.ticks_ms(), int(seconds * 1000))
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        r = kbd.read(20)
        if r:
            reports.append(bytes(r).hex())
    kbd.close()
    return {"device": "%s %s" % (dev.manufacturer, dev.product), "interface": kbd.interface,
            "protocol": kbd.protocol, "reports": reports}
