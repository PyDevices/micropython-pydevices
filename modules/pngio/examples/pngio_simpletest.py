# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Save a picture as a PNG, then load it back.

Draws a few coloured stripes into an RGB565 buffer, compresses them with
``pngio.PngEncoder`` and writes the PNG to a file. Then it decodes the
file with ``pngio.PngDecoder`` and checks that every pixel came back
exactly, since PNG, unlike JPEG, loses nothing.

Needs: a MicroPython firmware built with pngio (``--modules pngio`` or
``all``). On CPython, pydevices-desktop's ``pngio`` gives the same API
over Pillow.

Run it:

    mpremote run pngio_simpletest.py
    mpremote cp :stripes.png .

Tested on: the MicroPython unix port, built with ``--modules all``, and
CPython with pydevices-desktop on Linux and Windows.
"""

import pngio

# Change these to match your setup.
WIDTH = 120
HEIGHT = 80
FILENAME = "stripes.png"

# RGB565 colours: red, green, blue and white.
COLORS = (0xF800, 0x07E0, 0x001F, 0xFFFF)

# Draw four vertical stripes.
picture = bytearray(WIDTH * HEIGHT * 2)
for y in range(HEIGHT):
    for x in range(WIDTH):
        color = COLORS[x * len(COLORS) // WIDTH]
        i = (y * WIDTH + x) * 2
        picture[i] = color & 0xFF
        picture[i + 1] = color >> 8

# Compress it and save the file. Flat colours compress well.
png = pngio.PngEncoder().encode(picture, WIDTH, HEIGHT)
with open(FILENAME, "wb") as f:
    f.write(png)
print("Encoded %d bytes of pixels to %d bytes, saved as %s" % (len(picture), len(png), FILENAME))

# Load the file into a new buffer.
decoder = pngio.PngDecoder()
width, height = decoder.open(FILENAME)
loaded = bytearray(width * height * 2)
decoder.decode(loaded)

# Compare the two pictures.
if loaded == picture:
    print("Decoded %dx%d, every pixel matches" % (width, height))
else:
    print("Decoded %dx%d, but the pixels differ" % (width, height))
