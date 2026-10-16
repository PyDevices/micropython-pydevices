# Mount the flash drive on the host port through usb.host.msc, list it, then
# write a small file, read it back, and remove it.
import os
import time

import usb.core
import vfs
from usb.host.msc import Drive


def run():
    usb.core.start()
    dev = None
    for _ in range(10):
        dev = usb.core.find(custom_match=Drive.match)
        if dev:
            break
        time.sleep_ms(500)
    t0 = time.ticks_ms()
    drive = Drive(dev)
    vfs.mount(vfs.VfsFat(drive), "/usb")
    mount_ms = time.ticks_diff(time.ticks_ms(), t0)
    listing = os.listdir("/usb")[:8]
    data = bytes(range(256)) * 64       # 16 KB
    t0 = time.ticks_ms()
    with open("/usb/_usbcore_check.bin", "wb") as f:
        f.write(data)
    write_ms = time.ticks_diff(time.ticks_ms(), t0)
    vfs.umount("/usb")
    vfs.mount(vfs.VfsFat(Drive(dev)), "/usb")
    t0 = time.ticks_ms()
    with open("/usb/_usbcore_check.bin", "rb") as f:
        back = f.read()
    read_ms = time.ticks_diff(time.ticks_ms(), t0)
    os.remove("/usb/_usbcore_check.bin")
    gone = "_usbcore_check.bin" not in os.listdir("/usb")
    vfs.umount("/usb")
    return {"drive": "%s %s" % (dev.manufacturer, dev.product), "blocks": drive.blocks, "mount_ms": mount_ms,
            "listing": listing, "write_16k_ms": write_ms, "read_16k_ms": read_ms, "same": back == data,
            "removed": gone}
