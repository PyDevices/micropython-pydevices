# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Zoom into the middle of the picture, in hardware.

``capture_scaled(..., src_rect=(x, y, w, h))`` crops the camera frame and
scales the crop to fill the panel in one pass of the P4's Pixel Processing
Accelerator, so digital zoom costs no more than the plain preview. The
program steps from 1x to 4x and back, keeping the panel's shape, and prints
the crop and the frame rate at each step. Success is the picture on the
panel moving closer at each step with the frame rate holding steady.

Needs: an ESP32-P4 board with a MIPI-CSI camera and a display, both set up
in board_config, and firmware built with cameraif and ppa.

Run it:

    mpremote run camera_zoom.py

Tested on: not yet run on a board.
"""

import time

import board_config
from board_config import display_drv as display

# Change these to match your setup.
ZOOMS = (1, 1.5, 2, 3, 4, 2, 1)  # the steps, as magnification
HOLD_MS = 2000  # how long each step stays on the panel
SWAP = False  # True for a panel that takes RGB565 big-endian (most SPI panels)

cam = board_config.camera
framebuffer = display.framebuffers()[0]
frame_w, frame_h = cam.size()[:2]


def crop_for(zoom):
    """The centred block of the frame to show at this zoom, panel-shaped."""
    # The largest panel-shaped block the frame holds is 1x.
    if frame_w * display.height >= frame_h * display.width:
        h = frame_h
        w = frame_h * display.width // display.height
    else:
        w = frame_w
        h = frame_w * display.height // display.width
    # Zooming in shrinks it; even sizes keep the PPA's work simple.
    w = int(w / zoom) & ~1
    h = int(h / zoom) & ~1
    return ((frame_w - w) // 2, (frame_h - h) // 2, w, h)


try:
    for zoom in ZOOMS:
        crop = crop_for(zoom)
        frames = 0
        start = time.ticks_ms()
        # Show this zoom for a while, counting the frames that reach the panel.
        while time.ticks_diff(time.ticks_ms(), start) < HOLD_MS:
            if cam.capture_scaled(framebuffer, display.width, display.height,
                                  src_rect=crop, swap=SWAP, timeout=500) is None:
                continue
            display.show()
            frames += 1
        elapsed = time.ticks_diff(time.ticks_ms(), start)
        print("%.1fx: crop %s, %.1f fps" % (zoom, crop, frames * 1000 / elapsed))
finally:
    # Release the camera for the next program.
    cam.deinit()
