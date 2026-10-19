# usb.host.video on usb.core: open the hosted camera, stream, decode a frame.
import time

import usb.core
from usb.host.video import Camera

PLANT = False  # the proof matrix's host leg: the last frame's JPEG header is lost


def run(seconds=3, max_size=(320, 240)):
    usb.core.start(classes=("uvc",))
    dev = None
    for _ in range(40):
        dev = usb.core.find(custom_match=Camera.match)
        if dev:
            break
        time.sleep_ms(250)
    cam = Camera(dev, max_size=max_size, in_limit=600)
    buf = bytearray(cam.frame_bytes)
    got = last = 0
    with cam:
        t = time.ticks_ms()
        end = time.ticks_add(t, seconds * 1000)
        while time.ticks_diff(end, time.ticks_ms()) > 0:
            n = cam.read(buf)
            if n:
                got += 1
                last = n
            else:
                time.sleep_ms(2)
        secs = time.ticks_diff(time.ticks_ms(), t) / 1000
    out = {"camera": "%s %s" % (dev.manufacturer, dev.product), "mode": (cam.encoding, cam.width, cam.height, cam.fps),
           "frames": got, "fps": round(got / secs, 1), "last": last, "offers": cam.modes()}
    if PLANT and last:
        buf[0:2] = b"\x00\x00"
    if cam.encoding == "mjpeg" and last:
        import jpegio
        d = jpegio.JpegDecoder()
        d.open(memoryview(buf)[:last])
        out["decoded"] = (d.width, d.height)
    return out
