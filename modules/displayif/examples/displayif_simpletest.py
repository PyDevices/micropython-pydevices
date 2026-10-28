# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Draw straight into a MIPI-DSI panel's framebuffer.

displayif's ``mipidsi.Display`` keeps the panel's picture in a buffer in
memory and sends it to the panel. This example wraps that buffer in a
pygraphics ``FrameBuffer``, draws a few shapes and some text, and calls
``refresh()`` to show them.

Most programs use the display through ``board_config.display_drv`` and
never touch this layer. Come here when you bring up a new panel, or want
the fastest path to the pixels.

Needs: an ESP32-P4 board with a MIPI-DSI panel, firmware built with
displayif and pygraphics, and a ``board_config.py`` that creates the
``mipidsi.Display`` as ``fb``, as the Waveshare ESP32-P4 board configs do.

Run it:

    mpremote run displayif_simpletest.py

Tested on: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B (720x720), MicroPython
1.30 preview built with ``--modules all``.
"""

import board_config
import pygraphics
from pygraphics import RGB565, FrameBuffer

# RGB565 colours.
BLACK = 0x0000
RED = 0xF800
GREEN = 0x07E0
BLUE = 0x001F
WHITE = 0xFFFF

# The panel, as board_config set it up.
display = board_config.fb
width, height = display.width, display.height

# Wrap the panel's own buffer, so drawing on canvas draws on the panel.
canvas = FrameBuffer(memoryview(display), width, height, RGB565)

# Draw: a black background, three shapes and a line of text.
canvas.fill(BLACK)
canvas.fill_rect(40, 40, width // 3, height // 4, RED)
canvas.circle(width // 2, height // 2, height // 6, GREEN, f=True)
canvas.round_rect(width - 40 - width // 3, height - 40 - height // 4, width // 3, height // 4, 24, BLUE, f=True)
pygraphics.text16(canvas, "Hello from displayif", 40, height - 30, WHITE)

# Send the finished picture to the panel.
display.refresh()
print("Drew on the %dx%d panel" % (width, height))
