# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Take a photo and save it as a JPEG.

Captures one still from the board's camera, compresses it with the
ESP32-P4's hardware JPEG encoder, and writes it to the board's filesystem.
The program prints the file's size and how much smaller it is than the
raw image.

Needs: an ESP32-P4 board with a MIPI-CSI camera, set up in board_config,
and firmware built with cameraif.

Run it:

    mpremote run camera_snapshot.py
    mpremote cp :snapshot.jpg .

Tested on: Waveshare ESP32-P4-WIFI6-DEV-KIT with an OV5647 camera.
"""

import board_config

# Change these to match your setup.
FILENAME = "snapshot.jpg"
QUALITY = 85  # 1 to 100; higher is sharper and larger


def main():
    cam = board_config.camera
    try:
        # Let auto exposure settle. The first frames show whatever the
        # sensor saw before, often black.
        for _ in range(5):
            cam.frame(500)

        # Capture one frame and encode it as a JPEG.
        img = cam.capture_jpeg(QUALITY)
        if img is None:
            print("no frame")
            return

        # Check the start and end markers, so a cut-short encode
        # isn't saved as a broken file.
        if img[:2] != b"\xff\xd8" or img[-2:] != b"\xff\xd9":
            print("encoder produced %d bytes that are not a JPEG" % len(img))
            return

        # Save it and report the size.
        with open(FILENAME, "wb") as f:
            f.write(img)
        w, h, raw = cam.size()
        print("%s: %dx%d, %d bytes (%.1f:1)" % (FILENAME, w, h, len(img), raw / len(img)))
    finally:
        # Release the camera for the next program.
        cam.deinit()


if __name__ == "__main__":
    main()
