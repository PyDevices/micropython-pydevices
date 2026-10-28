# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Find a coloured shape in a picture.

Draws a red rectangle and a blue circle on a blank picture with the image
module, then asks find_blobs for everything red and prints where it is.
It saves the picture as a JPEG, so you can look at it yourself. With a
camera, the same find_blobs call finds red things in its pictures.

Needs: a MicroPython firmware built with vision (it's opt-in: name it in
the build), on an ESP32-S3 or ESP32-P4. No camera or display.

Run it:
    mpremote run vision_simpletest.py
    mpremote cp :shapes.jpg .

Tested on: a Waveshare ESP32-P4-WIFI6-Touch-LCD-4B on MicroPython 1.30
preview.
"""

import image

# Change these to match your setup.
WIDTH = 320
HEIGHT = 240
FILENAME = "shapes.jpg"

# Red, as find_blobs sees colour: lightness from 30 to 100, and well towards
# red on both of the LAB colour space's colour axes (15 to 127).
RED = (30, 100, 15, 127, 15, 127)

# Draw a red rectangle and a blue circle on a black picture.
picture = image.Image(WIDTH, HEIGHT, image.RGB565)
picture.clear()
picture.draw_rectangle(100, 60, 80, 50, color=(255, 0, 0), fill=True)
picture.draw_circle(40, 40, 20, color=(0, 0, 255), fill=True)

# Find every red patch of at least 100 pixels.
for blob in picture.find_blobs([RED], pixels_threshold=100, area_threshold=100):
    print("Found red at x=%d, y=%d, %d wide and %d high" % blob.rect())

# Save the picture as a JPEG.
jpeg = picture.to_jpeg(quality=85, copy=True)
with open(FILENAME, "wb") as f:
    f.write(jpeg.bytearray())
print("Saved", FILENAME)
