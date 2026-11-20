# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Keep a frame's pixels out of the garbage collector's way.

Makes the same 800x480 RGB565 frame twice, once in a bytearray and once in
an offheap Buffer, and times a garbage collection with each alive. The
collector reads every word of the bytearray, and skips the Buffer's bytes.
The program prints both times; the second is the one an animation feels.

Needs: firmware built with offheap (ESP32, or the unix, Windows or browser
build) and pygraphics. On a board, PSRAM for the bytearray.

Run it:

    mpremote run offheap_simpletest.py

Tested on: unix MicroPython 1.29 built with offheap and pygraphics.
"""

import gc
import time

import offheap
from pygraphics import RGB565, FrameBuffer

# Change these to match your display.
WIDTH = 800
HEIGHT = 480


# Draw something into a frame, then time one collection.
def draw_and_collect(pixels):
    frame = FrameBuffer(pixels, WIDTH, HEIGHT, RGB565)
    frame.fill(0x001F)
    frame.fill_rect(WIDTH // 4, HEIGHT // 4, WIDTH // 2, HEIGHT // 2, 0xFFE0)
    start = time.ticks_us()
    gc.collect()
    return time.ticks_diff(time.ticks_us(), start) / 1000


# The same frame in the heap, then outside it.
in_heap = draw_and_collect(bytearray(WIDTH * HEIGHT * 2))
gc.collect()
pixels = offheap.Buffer(WIDTH * HEIGHT * 2)
outside = draw_and_collect(pixels)
print("collection with a bytearray frame: %.1f ms" % in_heap)
print("collection with a Buffer frame:    %.1f ms" % outside)

# Give the bytes back now rather than at the next collection.
pixels.deinit()
