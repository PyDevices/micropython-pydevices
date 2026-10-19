# The S3 hosts a UVC camera through usb.host.video, streams for a while,
# and saves the last whole frame to /frame.bin.
import time

import usb.core
from usb.host.video import Camera

IN_LIMIT = 600


def run(seconds=6, max_w=10000, max_h=10000):
    usb.core.start(classes=("uvc",))
    dev = None
    end = time.ticks_add(time.ticks_ms(), 15000)
    while dev is None and time.ticks_diff(end, time.ticks_ms()) > 0:
        dev = usb.core.find(custom_match=Camera.match)
        if dev is None:
            time.sleep_ms(250)
    if dev is None:
        usb.core.stop()
        return "no camera"
    try:
        # The largest mode up to max_w x max_h whose payload fits IN_LIMIT.
        cam = Camera(dev, max_size=(max_w, max_h), in_limit=IN_LIMIT)
    except ValueError as e:
        usb.core.stop()
        return "no mode fits", str(e)
    buf = bytearray(cam.frame_bytes)
    got, last = 0, 0
    with cam:
        t0 = time.ticks_ms()
        end = time.ticks_add(t0, seconds * 1000)
        while time.ticks_diff(end, time.ticks_ms()) > 0:
            n = cam.read(buf)
            if n:
                got += 1
                last = n
            else:
                time.sleep_ms(5)
        secs = time.ticks_diff(time.ticks_ms(), t0) / 1000
        stats = cam.stats()
    with open("/frame.bin", "wb") as f:
        f.write(buf[:last])
    usb.core.stop()
    return {"mode": "%s %dx%d @ %s fps" % (cam.encoding, cam.width, cam.height, cam.fps),
            "payload": cam.payload, "frames": got, "fps": round(got / secs, 1),
            "last_bytes": last, "stats": stats}
