# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Draw one SVG icon at two sizes.

Parses a small heart icon with ``svgio.SvgDecoder``, draws its shape at
16 and 32 pixels wide, and prints each one as text, so you can see it scale
without a screen. The same ``decode()`` call fills an RGB565 buffer for a
display.

Needs: a MicroPython firmware built with svgio (``--modules svgio`` or
``all``). On CPython, pydevices-desktop's ``svgio`` gives the same API
over resvg.

Run it:

    mpremote run svgio_simpletest.py

Tested on: the MicroPython unix port, built with ``--modules svgio``.
"""

import svgio

# Change these to match your setup.
SIZES = (16, 32)

ICON = b"""<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24">
  <path fill="#e0245e" d="M12 21 C5 15 2 11.5 2 8 A5 5 0 0 1 12 6 A5 5 0 0 1 22 8 C22 11.5 19 15 12 21 Z"/>
</svg>"""

# Darker characters for more coverage.
SHADES = " .:-=+*#%@"

decoder = svgio.SvgDecoder()
for size in SIZES:
    width, height = decoder.open(ICON, width=size)
    alpha = bytearray(width * height)
    decoder.decode(None, alpha)
    print("%dx%d:" % (width, height))
    for y in range(height):
        row = alpha[y * width : (y + 1) * width]
        print("".join(SHADES[a * (len(SHADES) - 1) // 255] for a in row))
