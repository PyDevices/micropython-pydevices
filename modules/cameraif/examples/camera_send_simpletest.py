# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Take pictures with the camera and send them to another screen.

The board connects to VIEWER_HOST over TCP. Each time the viewer sends a
byte to ask for a picture, the board captures a JPEG in hardware and sends
it as a 4-byte length followed by the JPEG. It prints how many it sent and
how fast.

The viewer can be another board running jpegio_viewer_simpletest.py, or a
computer: anything that sends a byte, then reads the length and that many
bytes, can save or show the pictures.

Needs: a MicroPython firmware built with cameraif, on a board whose
board_config has a camera (an ESP32-P4 with a MIPI-CSI camera), already
connected to Wi-Fi. Start the viewer first.

Run it:
    mpremote run camera_send_simpletest.py

Tested on: Waveshare ESP32-P4 boards with an OV5647 or IMX219 camera on
MicroPython 1.30 preview, sending to an ESP32-S3 board running
jpegio_viewer_simpletest.py. Over Wi-Fi between those two, a picture can
take ten seconds or more to arrive.
"""

import socket
import struct
import time

import board_config

# Change these to match your setup.
VIEWER_HOST = "192.168.1.159"
VIEWER_PORT = 8080
QUALITY = 60  # 1 to 100; lower is smaller and quicker to send
PICTURES = 10

# Open the camera.
camera = board_config.camera
width, height = camera.size()[:2]
print("Camera %s, %dx%d" % (camera.sensor()[0], width, height))

# Connect to the viewer.
viewer = socket.socket()
viewer.connect(socket.getaddrinfo(VIEWER_HOST, VIEWER_PORT)[0][-1])
print("Sending to %s, port %d" % (VIEWER_HOST, VIEWER_PORT))

# Wait for the viewer to ask, then take a picture and send its length and
# the JPEG. Asking first means every picture is fresh, however long the
# viewer takes to show the last one. Close the connection and the camera at
# the end, or on Ctrl-C.
try:
    sent = 0
    start = time.ticks_ms()
    while sent < PICTURES:
        if not viewer.read(1):
            break  # the viewer hung up
        jpeg = None
        while jpeg is None:  # no frame ready yet
            jpeg = camera.capture_jpeg(QUALITY)
        viewer.write(struct.pack(">I", len(jpeg)))
        viewer.write(jpeg)
        sent += 1
        print("Sent picture %d, %d bytes" % (sent, len(jpeg)))
    seconds = time.ticks_diff(time.ticks_ms(), start) / 1000
    print("Sent %d pictures in %.1f s, %.1f a second" % (sent, seconds, sent / seconds))
finally:
    viewer.close()
    camera.deinit()
