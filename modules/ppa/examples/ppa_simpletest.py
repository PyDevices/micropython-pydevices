# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Scale and rotate a small picture, and print the result.

Builds a 4x2 picture with a different colour in each pixel, then uses
``ppa.srm`` to double its size and to turn it a quarter turn. Each
result is printed as a grid of letters, one per pixel, so you can see
where every pixel went.

On the ESP32-P4 the chip's Pixel Processing Accelerator does the work.
Everywhere else the same calls run in software, with the same results.

Needs: a MicroPython firmware built with ppa (``--modules ppa`` or
``all``).

Run it:

    mpremote run ppa_simpletest.py

Tested on: the MicroPython unix port, built with ``--modules all``.
"""

import ppa

# Eight RGB565 colours, and a letter to print for each.
COLORS = (
    (0xF800, "R"), (0x07E0, "G"), (0x001F, "B"), (0xFFFF, "W"),
    (0xFFE0, "Y"), (0x07FF, "C"), (0xF81F, "M"), (0x0000, "K"),
)
LETTERS = dict(COLORS)


def show(title, buf, width, height):
    print(title)
    for y in range(height):
        row = ""
        for x in range(width):
            i = (y * width + x) * 2
            row += LETTERS.get(buf[i] | (buf[i + 1] << 8), "?")
        print("  " + row)


# Build a 4x2 picture, one colour per pixel.
width, height = 4, 2
source = bytearray()
for color, _ in COLORS:
    source += bytes((color & 0xFF, color >> 8))
show("The picture, 4x2:", source, width, height)

# Scale it to twice the size. Each pixel becomes a 2x2 block.
big = bytearray(width * 2 * height * 2 * 2)
ppa.srm(source, width, height, big, width * 2, height * 2)
show("Scaled to 8x4:", big, width * 2, height * 2)

# Turn it a quarter turn counter-clockwise. Width and height swap.
turned = bytearray(width * height * 2)
ppa.srm(source, width, height, turned, height, width, rotate=90)
show("Turned 90 degrees, now 2x4:", turned, height, width)
