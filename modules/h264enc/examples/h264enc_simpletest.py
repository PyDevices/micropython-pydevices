# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Encode two seconds of moving pictures as an H.264 video file.

Draws 60 frames of a white square crossing a blue background, encodes
each one on the ESP32-P4's hardware H.264 encoder, and writes the video
to ``clip.h264``. Copy the file to your computer and play it with
ffplay or VLC.

Needs: an ESP32-P4 board, and firmware built with h264enc and ppa
(``--modules all`` has both).

Run it:

    mpremote run h264enc_simpletest.py
    mpremote cp :clip.h264 .
    ffplay clip.h264

Tested on: Waveshare ESP32-P4 board with a 720x720 panel, MicroPython 1.30
preview with ``--modules all``.
"""

import h264enc

# Change these to match your setup.
WIDTH = 320  # a multiple of 16
HEIGHT = 240  # a multiple of 16
FPS = 30
FRAMES = 60
BOX = 48
FILENAME = "clip.h264"

# RGB565 colours, in the board's byte order.
BLUE = b"\x1f\x00"
WHITE = b"\xff\xff"

# Make the background once, and a frame buffer to draw into.
background = BLUE * (WIDTH * HEIGHT)
frame = bytearray(background)
box_row = WHITE * BOX

# Start the encoder: one keyframe every second.
encoder = h264enc.Encoder(WIDTH, HEIGHT, fps=FPS, gop=FPS)

with open(FILENAME, "wb") as f:
    for n in range(FRAMES):
        # Draw the frame: background first, then the square.
        frame[:] = background
        x = n * (WIDTH - BOX) // FRAMES
        y = (HEIGHT - BOX) // 2
        for row in range(y, y + BOX):
            start = (row * WIDTH + x) * 2
            frame[start : start + BOX * 2] = box_row

        # Encode it, and write the result straight to the file.
        f.write(encoder.encode(frame))

# Report what the encoder did, then give it back.
stats = encoder.stats()
encoder.close()
print("Wrote %d frames (%d keyframes), %d bytes, to %s" % (stats["frames"], stats["keyframes"], stats["bytes"], FILENAME))
