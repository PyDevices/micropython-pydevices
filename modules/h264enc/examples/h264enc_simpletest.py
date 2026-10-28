# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Encode two seconds of moving pictures as an H.264 video file.

Draws 60 frames of a white square crossing a blue background, encodes
each one on the ESP32-P4's hardware H.264 encoder, and writes the video
to ``clip.h264``. Copy the file to your computer and play it with
ffplay or VLC.

Needs: an ESP32-P4 board, and firmware built with h264enc, ppa and
pygraphics (``--modules all`` has them all).

Run it:

    mpremote run h264enc_simpletest.py
    mpremote cp :clip.h264 .
    ffplay clip.h264

Tested on: an ESP32-P4 board with ESP32-C6 Wi-Fi (``ESP32_GENERIC_P4``,
``C6_WIFI``), MicroPython 1.30 preview built with ``--modules all``.
"""

import h264enc
from pygraphics import RGB565, FrameBuffer

# Change these to match your setup.
WIDTH = 320  # a multiple of 16
HEIGHT = 240  # a multiple of 16
FPS = 30
FRAMES = 60
BOX = 48
FILENAME = "clip.h264"

# RGB565 colours.
BLUE = 0x001F
WHITE = 0xFFFF

# A frame buffer to draw into, and pygraphics to draw on it.
frame = bytearray(WIDTH * HEIGHT * 2)
canvas = FrameBuffer(frame, WIDTH, HEIGHT, RGB565)

# Start the encoder: one keyframe every second.
encoder = h264enc.Encoder(WIDTH, HEIGHT, fps=FPS, gop=FPS)

with open(FILENAME, "wb") as f:
    for n in range(FRAMES):
        # Draw the frame: background first, then the square.
        canvas.fill(BLUE)
        x = n * (WIDTH - BOX) // FRAMES
        y = (HEIGHT - BOX) // 2
        canvas.fill_rect(x, y, BOX, BOX, WHITE)

        # Encode it, and write the result straight to the file.
        f.write(encoder.encode(frame))

# Report what the encoder did, then give it back.
stats = encoder.stats()
encoder.close()
print("Wrote %d frames (%d keyframes), %d bytes, to %s" % (stats["frames"], stats["keyframes"], stats["bytes"], FILENAME))
