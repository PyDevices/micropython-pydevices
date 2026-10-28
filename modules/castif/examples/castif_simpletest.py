# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Stream live video from the board to any player on your network.

Draws a white square crossing a blue background and hands each frame to
``castif.Hls``. That runs on the ESP32-P4's second core: it encodes the
frames to H.264, cuts them into HLS segments and serves them over HTTP,
so the drawing loop here only draws. Open the URL it prints in VLC or
ffplay to watch, about four seconds behind.

Needs: an ESP32-P4 board already connected to Wi-Fi, and firmware built
with castif, h264enc, tsmux, ppa and pygraphics (``--modules all`` has
them all).

Run it:

    mpremote run castif_simpletest.py

then, on a computer on the same network:

    ffplay http://<board address>:8090/stream.m3u8

Tested on: an ESP32-P4 board with ESP32-C6 Wi-Fi (``ESP32_GENERIC_P4``,
``C6_WIFI``), MicroPython 1.30 preview built with ``--modules all``.
"""

import time

import castif
import network
from pygraphics import RGB565, FrameBuffer

# Change these to match your setup.
WIDTH = 640  # a multiple of 16
HEIGHT = 480  # a multiple of 16
FPS = 20
SECONDS = 30
BOX = 96

# RGB565 colours.
BLUE = 0x001F
WHITE = 0xFFFF

# A frame buffer to draw into, and pygraphics to draw on it.
frame = bytearray(WIDTH * HEIGHT * 2)
canvas = FrameBuffer(frame, WIDTH, HEIGHT, RGB565)

# Start serving. The stream's address is the board's own.
hls = castif.Hls(WIDTH, HEIGHT, fps=FPS)
hls.start()
address = network.WLAN(network.STA_IF).ifconfig()[0]
print("Streaming for %d seconds at http://%s:8090/stream.m3u8" % (SECONDS, address))

try:
    frames = FPS * SECONDS
    for n in range(frames):
        # Draw the frame: background first, then the square.
        canvas.fill(BLUE)
        x = n * 8 % (WIDTH - BOX)
        y = (HEIGHT - BOX) // 2
        canvas.fill_rect(x, y, BOX, BOX, WHITE)

        # Offer the finished frame. If the encoder is still busy with
        # the last one, it skips this one and the stream carries on.
        hls.offer(frame)
        time.sleep_ms(1000 // FPS)
finally:
    # Report what was sent, then stop serving and free the encoder.
    stats = hls.stats()
    hls.stop()
    hls.close()
    print("Encoded %d frames, served %d requests" % (stats["frames"], stats["requests"]))
