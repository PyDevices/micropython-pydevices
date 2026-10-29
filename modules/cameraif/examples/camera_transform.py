# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Scale, rotate, mirror and crop the camera onto the panel.

``capture_scaled()`` hands the frame to the Pixel Processing Accelerator,
which rotates and mirrors in the same pass as the scale at no extra cost. The
destination is a whole picture plus the rectangle to write inside it, which is
exactly what a panel framebuffer is. The program cycles through six
orientations, with a cropped inset of the middle quarter of the panel after
each, so the effect is clear on the glass.

Needs: an ESP32-P4 board with a MIPI-CSI camera and a display, both set up
in board_config, and firmware built with cameraif.

Run it:

    mpremote run camera_transform.py
"""

import time

import board_config
from board_config import display_drv as display


# Each entry is the rotation in degrees, whether to mirror, and a label.
ORIENTATIONS = (
    (0, False, "normal"),
    (90, False, "rotate 90"),
    (180, False, "rotate 180"),
    (270, False, "rotate 270"),
    (0, True, "mirrored"),
    (180, True, "rotate 180 + mirror"),
)


def main(hold_s=2):
    # Open the camera and the panel's framebuffer.
    cam = board_config.camera
    fb = display.framebuffers()[0]
    print("sensor %s %dx%d -> panel %dx%d"
          % ((cam.sensor()[0],) + cam.size()[:2] + (display.width, display.height)))

    try:
        # For each orientation, fill the panel for a couple of seconds.
        for rotate, mirror, label in ORIENTATIONS:
            print(label)
            end = time.ticks_add(time.ticks_ms(), int(hold_s * 1000))
            while time.ticks_diff(end, time.ticks_ms()) > 0:
                # Full panel; crop example: pass x,y,w,h to write into a
                # quadrant instead -- same call, smaller rectangle.
                if cam.capture_scaled(fb, display.width, display.height,
                                      rotate=rotate, mirror=mirror,
                                      timeout=500) is None:
                    continue
                display.show()
            # And a cropped inset: centre quarter of the panel.
            w, h = display.width // 2, display.height // 2
            x, y = (display.width - w) // 2, (display.height - h) // 2
            print("  + cropped inset %dx%d at (%d,%d)" % (w, h, x, y))
            display.fill(0)
            if cam.capture_scaled(fb, display.width, display.height,
                                  x=x, y=y, w=w, h=h,
                                  rotate=rotate, mirror=mirror,
                                  timeout=500) is not None:
                display.show()
            time.sleep_ms(800)
    except KeyboardInterrupt:
        print("stopped")
    finally:
        cam.deinit()


if __name__ == "__main__":
    main()
