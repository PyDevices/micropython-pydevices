# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Tell whether a picture has a person in it.

Runs TensorFlow Lite Micro's person_detection model, a 300 KB MobileNet that
looks at a 96x96 grey picture and scores "person" against "no person". The
picture is a JPEG or PNG file, or a frame from the board's camera. The
program crops the middle square, shrinks it to 96x96, prints both scores
and the answer, and how long the model took.

Needs: an ESP32-S3 or ESP32-P4 board with firmware built with tflite and
jpegio or pngio, person_detect.tflite from tflite-micro's
tensorflow/lite/micro/models folder, and a picture. For the camera, set
IMAGE to None: that needs firmware built with cameraif and a camera set up
in board_peripherals.

    mpftp put person_detect.tflite /person_detect.tflite
    mpftp put photo.jpg /photo.jpg

Run it:

    mpftp run tflite_person_detect.py

Tested on: an ESP32-P4 board on MicroPython v1.30.0-preview.160.gb70ae1aca6
(built 2026-10-29), with its camera.
"""

import time

import tflite

# Change these to match your setup.
MODEL = "/person_detect.tflite"
IMAGE = "/photo.jpg"  # a .jpg or .png file, or None for the camera
SIZE = 96  # the model's input is SIZE x SIZE grey


def load_image(path):
    # Decode a JPEG or PNG file into native-order RGB565.
    if path.endswith(".png"):
        import pngio

        decoder = pngio.PngDecoder()
    else:
        import jpegio

        decoder = jpegio.JpegDecoder()
    width, height = decoder.open(path)
    pixels = bytearray(width * height * 2)
    decoder.decode(pixels)
    return pixels, width, height


def take_photo():
    # Grab one frame from the camera, after auto exposure settles.
    import board_peripherals

    camera = board_peripherals.camera()
    for _ in range(10):
        frame = camera.frame(1000)
    width, height, _ = camera.size()
    pixels = bytearray(frame)
    camera.deinit()
    return pixels, width, height


# Load the model.
model = tflite.Model(MODEL)
print("model:", model.len, "bytes, arena", model.ram, "bytes")
scale, zero = model.input_scale[0], model.input_zero_point[0]

# Get the picture.
pixels, width, height = load_image(IMAGE) if IMAGE else take_photo()
print("picture: %dx%d" % (width, height))

# Crop the middle square and shrink it to SIZE x SIZE grey, one byte per
# pixel, quantised the way the model was trained: -1 to 1 for black to white.
side = min(width, height)
left, top = (width - side) // 2, (height - side) // 2
picks = [(i * side) // SIZE for i in range(SIZE)]
grey = bytearray(SIZE * SIZE)
for y in range(SIZE):
    row = (top + picks[y]) * width + left
    for x in range(SIZE):
        at = (row + picks[x]) * 2
        v = pixels[at] | pixels[at + 1] << 8
        r, g, b = (v >> 8) & 0xF8, (v >> 3) & 0xFC, (v << 3) & 0xF8
        luma = (r * 77 + g * 150 + b * 29) >> 8
        q = round((luma / 127.5 - 1) / scale) + zero
        grey[y * SIZE + x] = max(-128, min(127, q)) & 0xFF


def fill(buffer, shape, dtype):
    buffer[:] = grey


# Run the model. Its two outputs score "no person" and "person".
t0 = time.ticks_ms()
scores = model.predict([fill])[0][0]
took = time.ticks_diff(time.ticks_ms(), t0)
no_person, person = scores[0], scores[1]
print("person %d, no person %d: %s (%d ms)" % (person, no_person, "person" if person > no_person else "no person", took))
model.deinit()
