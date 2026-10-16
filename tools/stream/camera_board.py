# Board half of the webcam stream check: bring up usb-device-video's camera
# beside the console and keep its ring fed with colour bars (YUY2), so a
# computer that opens the camera sees eight vertical bars.
import time

import usb.device
from usb.device.video import CameraInterface

cam = None
# White, yellow, cyan, green, magenta, red, blue, black (Y, U, V).
BARS = ((235, 128, 128), (210, 16, 146), (170, 166, 16), (145, 54, 34),
        (106, 202, 222), (81, 90, 240), (41, 240, 110), (16, 128, 128))


def bars(width, height):
    row = bytearray(width * 2)
    for x in range(0, width, 2):
        y, u, v = BARS[x * 8 // width]
        row[x * 2:x * 2 + 4] = bytes((y, u, y, v))
    return bytes(row) * height


def start(width=160, height=120, fps=10):
    global cam, frame
    cam = CameraInterface(width=width, height=height, fps=fps)
    frame = bars(width, height)
    usb.device.get().init(cam, builtin_driver=True)
    return cam.frame_bytes


def run(seconds):
    """Feed whole frames at the camera's rate for this long."""
    period = 1000 // cam.fps
    sent = 0
    end = time.ticks_add(time.ticks_ms(), int(seconds * 1000))
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        t = time.ticks_ms()
        if cam.streaming and cam.wants_frame():
            if cam.write_frame(frame):
                sent += 1
        time.sleep_ms(max(1, period - time.ticks_diff(time.ticks_ms(), t)))
    stream, ring = cam.stats()
    return {"frames_written": sent, "stream": stream, "ring": ring, "streaming": cam.streaming}
