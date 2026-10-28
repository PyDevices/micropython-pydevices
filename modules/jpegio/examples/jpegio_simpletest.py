# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Encode a picture as a JPEG, then decode it back.

Draws a colour gradient into an RGB565 buffer, compresses it with
``jpegio.JpegEncoder``, writes the JPEG to a file, and decodes the file
again with ``jpegio.JpegDecoder``, at full size and at one eighth. It
prints the sizes and one pixel from each decode, so you can see the
picture survived the trip.

Needs: a MicroPython firmware built with jpegio (``--modules jpegio`` or
``all``). No display or camera. CircuitPython's own jpegio decodes but
doesn't encode, so this example is for MicroPython.

Run it:

    mpremote run jpegio_simpletest.py
    mpremote cp :gradient.jpg .

Tested on: the MicroPython unix port, built with ``--modules all``.
"""

import jpegio

# Change these to match your setup.
WIDTH = 64
HEIGHT = 48
QUALITY = 80  # 1 to 100; higher is sharper and larger
FILENAME = "gradient.jpg"


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


# Draw a gradient: red across, green down, a little blue everywhere.
picture = bytearray(WIDTH * HEIGHT * 2)
for y in range(HEIGHT):
    for x in range(WIDTH):
        color = rgb565(x * 255 // WIDTH, y * 255 // HEIGHT, 64)
        i = (y * WIDTH + x) * 2
        picture[i] = color & 0xFF
        picture[i + 1] = color >> 8

# Compress it to JPEG and save the file.
jpeg = jpegio.JpegEncoder(quality=QUALITY).encode(picture, WIDTH, HEIGHT)
with open(FILENAME, "wb") as f:
    f.write(jpeg)
print("Encoded %dx%d to %d bytes, saved as %s" % (WIDTH, HEIGHT, len(jpeg), FILENAME))

# Open the file and decode it at full size into a new buffer.
decoder = jpegio.JpegDecoder()
width, height = decoder.open(FILENAME)
decoded = bytearray(width * height * 2)
decoder.decode(decoded)
corner = decoded[-2] | (decoded[-1] << 8)
print("Decoded %dx%d; bottom-right pixel is 0x%04X" % (width, height, corner))

# Decode it again at one eighth of the size, the fastest way to get a preview.
decoder.open(FILENAME)
small = bytearray((width >> 3) * (height >> 3) * 2)
decoder.decode(small, 3)
print("Decoded a %dx%d preview" % (width >> 3, height >> 3))
