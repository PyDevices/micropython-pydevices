# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Read frames from the camera without copying them.

``frame()`` returns a memoryview of the buffer the DMA just filled, valid
until the next ``frame()`` or ``capture()`` on this camera. The program prints
``stats()`` every 30 frames, so you can see whether frames are arriving (the
interrupt count climbing) or being dropped. This is for when you want the
pixels in your own code. To show the camera on the board's own screen, see
``camera_preview.py``.

Needs: an ESP32-P4 board with a MIPI-CSI camera, set up in board_config,
and firmware built with cameraif.

Run it:

    mpremote run camera_frame.py
"""

import time

import board_config


def main(seconds=5):
    # Open the camera and say what it is.
    cam = board_config.camera
    print("sensor %s %dx%d" % ((cam.sensor()[0],) + cam.size()[:2]))

    # Read frames for a few seconds, reporting every 30th.
    frames = 0
    t0 = time.ticks_ms()
    deadline = time.ticks_add(t0, seconds * 1000)
    try:
        while time.ticks_diff(deadline, time.ticks_ms()) > 0:
            mv = cam.frame(500)
            if mv is None:
                continue
            frames += 1
            if frames % 30 == 0:
                st = cam.stats()
                dt = time.ticks_diff(time.ticks_ms(), t0)
                print("%d frames, %.1f fps, %d bytes; stats %r"
                      % (frames, frames * 1000 / dt, len(mv), st))
    except KeyboardInterrupt:
        pass
    finally:
        print("final stats", cam.stats())
        cam.deinit()


if __name__ == "__main__":
    main()
