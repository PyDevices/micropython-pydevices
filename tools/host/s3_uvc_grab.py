# The S3 hosts a UVC camera through usbif's host video, streams for a while,
# and saves the last whole frame to /frame.bin.
import time

import usbif.auto
from usbif import uvc

IN_LIMIT = 600


def run(seconds=6):
    host = usbif.auto.host(classes=("uvc",)).start()
    dev_id = None
    end = time.ticks_add(time.ticks_ms(), 15000)
    while dev_id is None and time.ticks_diff(end, time.ticks_ms()) > 0:
        found = host.find("uvc")
        dev_id = found[0].id if found else None
        time.sleep_ms(250)
    if dev_id is None:
        host.stop()
        return "no camera"
    blob = host.desc(dev_id)
    formats = uvc.formats(blob)
    alts = uvc.alt_settings(blob)
    picked = None
    for fmt in formats:
        for frame in fmt.frames:
            for interval in (frame.intervals or [0]):
                payload, frame_bytes = host.uvc_negotiate(dev_id, fmt.interface, fmt.index, frame.index, interval)
                alt = uvc.alt_for_payload(alts, payload) if payload <= IN_LIMIT else None
                if alt is not None:
                    picked = (fmt, frame, interval, payload, frame_bytes, alt)
                    break
            if picked:
                break
        if picked:
            break
    if picked is None:
        host.stop()
        return "no mode fits", [(f.encoding, [(fr.width, fr.height) for fr in f.frames]) for f in formats]
    fmt, frame, interval, payload, frame_bytes, alt = picked
    buf = bytearray(frame_bytes)
    host.uvc_open(dev_id, fmt.interface, alt.alt, alt.endpoint, alt.max_packet, frame_bytes)
    got, last = 0, 0
    t0 = time.ticks_ms()
    end = time.ticks_add(t0, seconds * 1000)
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        n = host.uvc_read_frame(buf)
        if n:
            got += 1
            last = n
        else:
            time.sleep_ms(5)
    secs = time.ticks_diff(time.ticks_ms(), t0) / 1000
    stats = host.uvc_stats()
    host.uvc_close()
    with open("/frame.bin", "wb") as f:
        f.write(buf[:last])
    host.stop()
    return {"mode": uvc.describe(fmt, frame, interval), "payload": payload, "frames": got,
            "fps": round(got / secs, 1), "last_bytes": last, "stats": stats}
