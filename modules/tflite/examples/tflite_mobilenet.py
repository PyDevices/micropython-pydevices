# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Name what a picture shows, from ImageNet's 1,000 classes.

Runs an int8 MobileNet-v2 over a JPEG or PNG file, or a frame from the
board's camera, and prints the five likeliest of ImageNet's classes ("golden
retriever", "zebra", "tabby"). The program crops the middle square, shrinks
it to 224x224 and normalises each colour the way the model was trained.

Needs: an ESP32-P4 board (or an ESP32-S3 with 8 MB of PSRAM) with firmware
built with tflite and jpegio or pngio, and two downloads: the model
mobilenet_v2_int8_channelwise.tflite from huggingface.co/litert-community/MobileNet-v2,
and ImageNetLabels.txt from download.tensorflow.org/data/ImageNetLabels.txt.
The model is 4 MB, so an SD card is the place for it. For the camera, set
IMAGE to None: that needs firmware built with cameraif and a camera set up
in board_peripherals.

    mpftp put mobilenet_v2_int8_channelwise.tflite /sdcard/mobilenet_v2.tflite
    mpftp put ImageNetLabels.txt /sdcard/ImageNetLabels.txt
    mpftp put photo.jpg /sdcard/photo.jpg

Run it:

    mpftp run tflite_mobilenet.py

Tested on: an ESP32-P4 board on MicroPython v1.30.0-preview.160.gb70ae1aca6
(built 2026-10-29), with its camera.
"""

import math
import os
import time

import board_peripherals
import tflite

# Change these to match your setup.
MODEL = "/sdcard/mobilenet_v2.tflite"
LABELS = "/sdcard/ImageNetLabels.txt"
IMAGE = "/sdcard/photo.jpg"  # a .jpg or .png file, or None for the camera
SIZE = 224  # the model's input is 3 planes of SIZE x SIZE
MEAN = (0.485, 0.456, 0.406)  # ImageNet's colour means and spreads
STD = (0.229, 0.224, 0.225)


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
    camera = board_peripherals.camera()
    for _ in range(10):
        frame = camera.frame(1000)
    width, height, _ = camera.size()
    pixels = bytearray(frame)
    camera.deinit()
    return pixels, width, height


# Mount the SD card, if the board has one and it isn't mounted yet.
try:
    os.mount(board_peripherals.sdcard(), "/sdcard")
except Exception:
    pass

# Load the model and the class names. The labels file starts with "background".
model = tflite.Model(MODEL)
print("model:", model.len, "bytes, arena", model.ram, "bytes")
with open(LABELS) as f:
    names = [line.strip() for line in f][1:]
scale, zero = model.input_scale[0], model.input_zero_point[0]
out_scale, out_zero = model.output_scale[0], model.output_zero_point[0]

# Look-up tables from a 5 or 6 bit colour value to the model's int8, so the
# loop below does no arithmetic per pixel.
tables = []
for channel, bits in ((0, 5), (1, 6), (2, 5)):
    top = (1 << bits) - 1
    tables.append(bytes(max(-128, min(127, round(((v * 255 // top) / 255 - MEAN[channel]) / STD[channel] / scale + zero))) & 0xFF for v in range(top + 1)))
red, green, blue = tables

# Get the picture, crop the middle square and shrink it into the model's
# three colour planes.
pixels, width, height = load_image(IMAGE) if IMAGE else take_photo()
print("picture: %dx%d" % (width, height))
side = min(width, height)
left, top = (width - side) // 2, (height - side) // 2
picks = [(i * side) // SIZE for i in range(SIZE)]
plane = SIZE * SIZE
planes = bytearray(3 * plane)
for y in range(SIZE):
    row = (top + picks[y]) * width + left
    out = y * SIZE
    for x in range(SIZE):
        at = (row + picks[x]) * 2
        v = pixels[at] | pixels[at + 1] << 8
        planes[out + x] = red[v >> 11]
        planes[plane + out + x] = green[(v >> 5) & 0x3F]
        planes[2 * plane + out + x] = blue[v & 0x1F]
del pixels


def fill(buffer, shape, dtype):
    buffer[:] = planes


# Run the model and turn its scores into probabilities.
t0 = time.ticks_ms()
scores = model.predict([fill])[0][0]
took = time.ticks_diff(time.ticks_ms(), t0)
logits = [(scores[i] - out_zero) * out_scale for i in range(len(names))]
peak = max(logits)
odds = [math.exp(v - peak) for v in logits]
total = sum(odds)
ranked = sorted(range(len(names)), key=lambda i: -odds[i])[:5]
for i in ranked:
    print("%-24s %.2f" % (names[i], odds[i] / total))
print("%d ms per inference" % took)
model.deinit()
