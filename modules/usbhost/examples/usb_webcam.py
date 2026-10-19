"""The board as a USB webcam: what its camera sees, on the computer.

A computer sees a camera (UVC) and any video app can open it: the Camera app
on Windows, Photo Booth or QuickTime on macOS, Cheese or ffplay on Linux.
cameraif captures each frame in the sensor mode nearest 4:3, the ESP32-P4's
PPA scales it, the P4's JPEG engine encodes it, and machine.USBDevice's
streaming endpoint sends it from the USB interrupt.

    mpftp run -d COM31 examples/usb_webcam.py

or from a program: ``import usb_webcam; usb_webcam.main()``, or
``main(packet=640)`` for a computer at full speed, which takes bigger packets
than the 512 every host (an ESP32-S3 included) takes.

Needs an ESP32-P4 with a MIPI camera and a board config whose
``board_peripherals.camera()`` opens it, a firmware with cameraif, ppa,
jpegio and machine.USBDevice's streaming endpoints, and micropython-lib's
``usb-device`` with usbhost's ``usb-device-video`` on the board.

MJPEG is the default, offered at 640x480, 320x240 and 160x120 for the
computer to choose: 15 frames a second asked on a full-speed port (about 12
arrive at 640x480 in a lit room) and 30 on a high-speed one.
``FORMAT = "yuy2"`` sends uncompressed frames instead, which every host takes
but which fill a full-speed port at 160x120.
The REPL's serial port stays up beside the camera. Ctrl-C stops it.
"""

import time

import board_peripherals
import machine
import uctypes
import usb.device
from usb.device.video import CameraInterface

FORMAT = "mjpeg"
QUALITY = 70

high_speed = machine.USBDevice().speed() == 2
# The sizes on offer, largest first; the computer picks one. A host that
# decodes in software (an ESP32-S3) does better asking for a smaller one.
if FORMAT == "mjpeg":
    SIZES, FPS = ((640, 480), (320, 240), (160, 120)), (30 if high_speed else 15)
else:
    SIZES, FPS = (((320, 240), (160, 120)), 30) if high_speed else (((160, 120),), 15)
WIDTH, HEIGHT = SIZES[0]
# One isochronous packet a millisecond at full speed, a microframe at high
# speed. High speed allows 1024 bytes. At full speed the default is 512, which
# every host takes, an ESP32-S3 (at most 600) included. A computer takes more:
# main(packet=640) is the most the ESP32's full-speed controller has FIFO room
# for beside the serial port (768 fails to open), and carries 640x480 MJPEG at
# about 12 frames a second.
PACKET = 1024 if high_speed else 512


def aligned(n):
    """A buffer the PPA writes by DMA: 64-byte aligned, whole cache lines."""
    n = (n + 63) & ~63
    raw = bytearray(n + 64)
    skip = -uctypes.addressof(raw) & 63
    return memoryview(raw)[skip:skip + n]


def open_4_3():
    """The camera in the sensor mode nearest 4:3 that's at least as big as the
    picture: the PPA scales a whole frame, it doesn't crop one."""
    cam = board_peripherals.camera()
    modes = [m for m in cam.formats() if m[1] >= WIDTH and m[2] >= HEIGHT]
    if not modes:
        return cam
    best = min(modes, key=lambda m: (abs(m[1] * 3 - m[2] * 4) / m[2], m[1] * m[2]))
    if cam.size()[:2] == best[1:3]:
        return cam
    cam.deinit()
    return board_peripherals.camera(format=best[0])


def main(packet=PACKET):
    """Run the webcam until Ctrl-C. ``packet`` is the bytes an isochronous
    packet carries; raise it for a host that takes more than 512."""
    cam = open_4_3()
    if FORMAT == "mjpeg":
        import jpegio

        encoder = jpegio.JpegEncoder(quality=QUALITY)
        rgb = aligned(WIDTH * HEIGHT * 2)
        uvc = CameraInterface(sizes=SIZES, fps=FPS, format="mjpeg", max_frame=WIDTH * HEIGHT // 3, packet=packet)
    else:
        frame = aligned(WIDTH * HEIGHT * 2)
        uvc = CameraInterface(sizes=SIZES, fps=FPS, packet=packet)
    usb.device.get().init(uvc, builtin_driver=True)
    print("webcam up: %s %s at %d fps; open it on the computer (Ctrl-C stops)"
          % (FORMAT, " ".join("%dx%d" % sz for sz in SIZES), FPS))
    sent = biggest = 0
    t0 = time.ticks_ms()
    try:
        while True:
            if uvc.streaming and uvc.wants_frame():
                w, h = uvc.width, uvc.height    # the size this session asked for
                if FORMAT == "mjpeg":
                    cam.capture_scaled(rgb, w, h)
                    jpeg = encoder.encode(rgb[:w * h * 2], w, h)
                    biggest = max(biggest, len(jpeg))
                    ok = len(jpeg) <= uvc.frame_bytes and uvc.write_frame(jpeg)
                else:
                    cam.capture_yuy2(frame, w, h)
                    ok = uvc.write_frame(frame[:w * h * 2])
                sent += 1 if ok else 0
            else:
                time.sleep_ms(2)
            if time.ticks_diff(time.ticks_ms(), t0) >= 5000:
                if sent:
                    print("%dx%d %.1f frames/s%s" % (uvc.width, uvc.height,
                                                     sent * 1000 / time.ticks_diff(time.ticks_ms(), t0),
                                                     ", largest JPEG %d bytes" % biggest if biggest else ""))
                sent = biggest = 0
                t0 = time.ticks_ms()
    finally:
        usb.device.get().init(builtin_driver=True)
        cam.deinit()


if __name__ == "__main__":
    main()
