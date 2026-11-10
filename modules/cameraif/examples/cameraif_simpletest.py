# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Open the camera, grab one frame and print what came back.

The shortest program that proves cameraif works: it names the sensor the
camera found, then waits for one frame from the sensor and prints its size.
If it prints a frame size, the sensor, the MIPI lanes and the capture path
are all working.

Needs: an ESP32-P4 board with a MIPI-CSI camera, set up in board_config,
and firmware built with cameraif.

Run it:

    mpremote run cameraif_simpletest.py

Tested on: Espressif ESP32-P4X-EYE with an OV2710 camera, MicroPython
v1.30.0-preview-161-ga8a0659f9.
"""

import board_config

camera = board_config.camera
try:
    # Ask the camera which sensor answered and how big its frames are.
    name, product_id = camera.sensor()
    width, height, frame_bytes = camera.size()
    print("sensor:", name, product_id)
    print("frame: %dx%d, %d bytes" % (width, height, frame_bytes))

    # Wait up to a second for one frame; the view is valid until the next call.
    frame = camera.frame(1000)
    if frame is None:
        print("no frame arrived")
    else:
        print("got a frame of %d bytes" % len(frame))
finally:
    # Release the camera for the next program.
    camera.deinit()
