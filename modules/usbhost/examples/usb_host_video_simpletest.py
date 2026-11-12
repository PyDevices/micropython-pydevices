# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Grab one frame from a USB camera and save it on the board.

The board is the USB host and the camera its device: a webcam, or anything
else that speaks the USB Video Class. ``usb.host.video.Camera`` negotiates the
largest mode the board's USB port can carry and streams frames. The program
waits for the first whole frame and writes it to a file. A frame in MJPEG,
which the camera prefers, is a JPEG file you can open as it is.
``uvc_display.py`` shows the stream on a screen.

Needs: a board with USB host support (firmware built with usbhost), the
``usb-host-video`` package, and a camera in the host port. A P4 hosts on a
port that supplies 5 V, such as the ESP32-P4-WIFI6-DEV-KIT's Type-A sockets.
Behind a hub on a P4, a full-speed camera needs ``FULL_SPEED = True``: a
high-speed hub can't reach one otherwise. A high-speed webcam wants it False.

Run it:

    mpftp run --follow examples/usb_host_video_simpletest.py
    mpftp get /frame.jpg frame.jpg

Success is the frame's size printed and a picture in ``frame.jpg``.

Tested on: Waveshare ESP32-P4-WIFI6-DEV-KIT on MicroPython
v1.30.0-preview.160.gb70ae1aca6 (built 2026-11-11), behind its onboard hub, with FULL_SPEED = True, hosting an Arduino GIGA R1 WiFi presenting a 160x120 MJPEG camera
(``usb.device.video``): the frame it saved was byte-identical to the JPEG the GIGA sent.
"""

import time

import usb.core
from usb.host.video import Camera

# Change these to match your setup.
FULL_SPEED = False
FILENAME = "/frame.jpg"
MAX_SIZE = (640, 480)  # the largest picture to ask for, as (width, height)

# Start the host and wait for a camera to enumerate.
usb.core.start(full_speed=FULL_SPEED, classes=("uvc",))
try:
    device = None
    while device is None:
        time.sleep_ms(250)
        device = usb.core.find(custom_match=Camera.match)
    camera = Camera(device, max_size=MAX_SIZE)
    print("Camera:", device.product)
    print("Mode:", camera.encoding, camera.width, "x", camera.height, "at", camera.fps, "fps")

    # Read until a whole frame arrives, then save it.
    frame = bytearray(camera.frame_bytes)
    camera.start()
    size = 0
    start = time.ticks_ms()
    while not size and time.ticks_diff(time.ticks_ms(), start) < 10000:
        size = camera.read(frame)
        time.sleep_ms(20)
    camera.stop()
    if size:
        with open(FILENAME, "wb") as file:
            file.write(memoryview(frame)[:size])
        print("Saved", size, "bytes to", FILENAME)
    else:
        print("No frame arrived")
finally:
    # Release the port for the next program.
    usb.core.stop()
