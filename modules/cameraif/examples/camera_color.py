# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Move contrast, saturation, brightness and hue on the live picture.

``cam.color()`` drives the ESP32-P4 ISP's colour block, which works on the
picture after gamma, on every sensor, so it is what a camera's picture
settings menu moves. The program shows the camera on the panel and steps
each setting through a few values in turn, printing each one, then puts the
picture back as it was. Success is the picture on the panel changing as the
printed values say: flatter and punchier, grey and vivid, darker and
lighter, then its colours turning round the wheel.

Needs: an ESP32-P4 board with a MIPI-CSI camera and a display, both set up
in board_config, and firmware built with cameraif and ppa.

Run it:

    mpremote run camera_color.py

Tested on: not yet run on a board.
"""

import time

import board_config
from board_config import display_drv as display

# Change these to match your setup.
HOLD_MS = 1500  # how long each value stays on the panel
SWAP = False  # True for a panel that takes RGB565 big-endian (most SPI panels)

# Each setting and the values to show, in the colour block's own units.
STEPS = (
    ("contrast", (64, 128, 200)),  # 0..255, 128 leaves it as it is
    ("saturation", (0, 128, 255)),  # 0..255, 0 is grey
    ("brightness", (-80, 0, 80)),  # -128..127
    ("hue", (0, 90, 180, 270)),  # degrees round the colour wheel
)

cam = board_config.camera
framebuffer = display.framebuffers()[0]


def show(ms):
    """Paint what the camera sees for ms milliseconds."""
    end = time.ticks_add(time.ticks_ms(), ms)
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        if cam.capture_scaled(framebuffer, display.width, display.height,
                              swap=SWAP, timeout=500) is not None:
            display.show()


try:
    # Keep the starting settings, to put them back at the end.
    start = cam.color()
    print("starting from", start)

    # Step each setting through its values, the others as they started.
    for name, values in STEPS:
        for value in values:
            print("%s = %d" % (name, value))
            cam.color(**{name: value})
            show(HOLD_MS)
        cam.color(**{name: start[name]})

    print("back to", cam.color(**start))
finally:
    # Release the camera for the next program.
    cam.deinit()
