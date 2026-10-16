"""The board as a USB webcam: what its camera sees, on the computer.

A computer sees a camera (UVC, uncompressed YUY2) and any video app can open
it: the Camera app on Windows, Photo Booth or QuickTime on macOS, Cheese or
ffplay on Linux. cameraif captures each frame, the ESP32-P4's PPA scales it
and converts it to YUY2, and machine.USBDevice's streaming endpoint sends it
from the USB interrupt.

    mpftp run -d COM31 examples/usb_webcam.py

Needs an ESP32-P4 with a MIPI camera and a board config whose
``board_peripherals.camera()`` opens it, a firmware with cameraif, ppa and
machine.USBDevice's streaming endpoints, and micropython-lib's
``usb-device`` with usbif's ``usb-device-video`` on the board.

The picture size follows the port's speed: full speed carries about 1 MB/s,
so 160x120 at 15 frames a second; high speed 320x240 at 30. The REPL's serial
port stays up beside the camera. Ctrl-C stops it.
"""

import time

import board_peripherals
import machine
import usb.device
from usb.device.video import CameraInterface

high_speed = machine.USBDevice().speed() == 2
WIDTH, HEIGHT, FPS = (320, 240, 30) if high_speed else (160, 120, 15)


def main():
    cam = board_peripherals.camera()
    uvc = CameraInterface(width=WIDTH, height=HEIGHT, fps=FPS)
    usb.device.get().init(uvc, builtin_driver=True)
    frame = bytearray(uvc.frame_bytes)
    print("webcam up: %dx%d at %d fps; open it on the computer (Ctrl-C stops)" % (WIDTH, HEIGHT, FPS))
    sent = 0
    t0 = time.ticks_ms()
    try:
        while True:
            if uvc.streaming and uvc.wants_frame():
                cam.capture_yuy2(frame, WIDTH, HEIGHT)
                if uvc.write_frame(frame):
                    sent += 1
            else:
                time.sleep_ms(2)
            if time.ticks_diff(time.ticks_ms(), t0) >= 5000:
                if sent:
                    print("%.1f frames/s" % (sent * 1000 / time.ticks_diff(time.ticks_ms(), t0)))
                sent = 0
                t0 = time.ticks_ms()
    finally:
        usb.device.get().init(builtin_driver=True)
        cam.deinit()


main()
