# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Stream a moving test pattern as H.264 over RTSP.

Draws a red square sliding across a picture, encodes each frame with the
ESP32-P4's hardware H.264 encoder, and serves the stream from an RTSP
server on the board. Success is the square moving in a player. The program
joins Wi-Fi from ``/lib/secrets.py`` (pydevices' ``wifi.py``), prints the
URL, and runs for ``SECONDS``.

Needs: an ESP32-P4 board with firmware built with vision, which carries the
``image``, ``h264`` and ``rtsp`` modules, and a ``secrets.py`` on the board
with your Wi-Fi details.

Run it as the board's ``main.py``, or with nothing attached to the USB port.
A command that outlasts ``mpftp``'s wait resets the board over USB, which
looks like the stream crashing. Then open the URL it prints in a player:

    ffplay rtsp://BOARD-IP:8554/
"""

import time

import h264
import image
import network
import rtsp
import wifi

# Join Wi-Fi and print the address to open in a player.
wifi.connect_from_secrets()
print("rtsp://%s:8554/" % network.WLAN(network.STA_IF).ifconfig()[0])

# Change these to match your setup.
W, H, FPS, SECONDS = 320, 240, 15, 90

# Make the picture to draw into, the encoder and the RTSP server.
img = image.Image(W, H, image.RGB565)
enc = h264.H264Encoder(W, H, fps=FPS)
srv = rtsp.RTSPServer(W, H, fps=FPS, listen_port=8554)
period = 1000 // FPS
t_end = time.ticks_add(time.ticks_ms(), SECONDS * 1000)
# Draw a square that moves a little each frame, encode it, send it, and
# sleep for whatever is left of the frame period.
n = 0
try:
    while time.ticks_diff(t_end, time.ticks_ms()) > 0:
        t0 = time.ticks_ms()
        img.clear()
        x = (n * 4) % (W - 40)
        img.draw_rectangle(x, 100, 40, 40, color=(255, 0, 0), fill=True)
        img.draw_string(8, 8, "frame %d" % n, color=(255, 255, 255), scale=2)
        srv.send(enc.encode(img))
        n += 1
        time.sleep_ms(max(0, period - time.ticks_diff(time.ticks_ms(), t0)))
finally:
    # Stop the server and release the encoder.
    srv.stop()
    enc.close()
