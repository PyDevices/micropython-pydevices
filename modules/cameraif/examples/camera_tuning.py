# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Compare the sensor's colour tuning with none at all.

The camera opens with its sensor's own colour tuning: a colour matrix (or a
table of them by the light's colour temperature), a gamma curve and the ISP
colour block's starting contrast and saturation. ``cam.tuning()`` reads it
and takes it back, so the program saves it, shows the picture with a plain
identity matrix and a straight gamma curve, then restores the saved tuning.
It prints the light's colour temperature as the camera reads it. Success is
a pale, flat picture in the middle stretch and natural colour either side.

Needs: an ESP32-P4 board with a MIPI-CSI camera and a display, both set up
in board_config, and firmware built with cameraif and ppa.

Run it:

    mpremote run camera_tuning.py

Tested on: not yet run on a board.
"""

import time

import board_config
from board_config import display_drv as display

# Change these to match your setup.
HOLD_MS = 4000  # how long each tuning stays on the panel
SWAP = False  # True for a panel that takes RGB565 big-endian (most SPI panels)

IDENTITY = (1, 0, 0, 0, 1, 0, 0, 0, 1)

cam = board_config.camera
framebuffer = display.framebuffers()[0]


def show(label):
    """Paint what the camera sees for a while, then report the light."""
    print(label)
    end = time.ticks_add(time.ticks_ms(), HOLD_MS)
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        if cam.capture_scaled(framebuffer, display.width, display.height,
                              swap=SWAP, timeout=500) is not None:
            display.show()
    print("  colour temperature:", cam.color_temp(), "K")


try:
    # Read what the sensor opened with, and keep it.
    name = cam.sensor()[0]
    saved = cam.tuning()
    table = saved["ccm_table"]
    print("%s: gamma %.2f, %s" % (name, saved["gamma"],
          "%d matrices by colour temperature" % len(table) if table else "one matrix"))
    print("colour block:", saved["color"])
    show("the sensor's tuning")

    # No correction: an identity matrix, a straight curve, the block neutral.
    cam.tuning(ccm=IDENTITY, ccm_table=None, gamma=1.0,
               color={"contrast": 128, "saturation": 128})
    show("no tuning")

    # Put the saved tuning back as it was.
    cam.tuning(**saved)
    show("the sensor's tuning again")
finally:
    # Release the camera for the next program.
    cam.deinit()
