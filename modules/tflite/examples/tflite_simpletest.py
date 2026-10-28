# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Run a TensorFlow Lite model: predict a sine wave.

Loads a tiny model that learned sin(x) for x from 0 to 2π, asks it for a
few values, and prints each prediction beside math.sin. The model works in
8-bit integers, so each x is scaled into the model's units on the way in
and back out again on the way out.

Needs: a MicroPython firmware built with tflite (it's opt-in: name it in
the build), and the model on the board. It is esp-vision's
models/tflite/sine/sine.tflite, from https://github.com/espressif/esp-vision.

Run it:
    mpremote cp sine.tflite :
    mpremote run tflite_simpletest.py

Tested on: a Waveshare ESP32-P4-WIFI6-Touch-LCD-4B on MicroPython 1.30
preview.
"""

import math

import tflite

# Change these to match your setup.
MODEL = "sine.tflite"
POINTS = 8

# Load the model, and read how it scales numbers into 8-bit integers.
model = tflite.Model(MODEL)
in_scale, in_zero = model.input_scale[0], model.input_zero_point[0]
out_scale, out_zero = model.output_scale[0], model.output_zero_point[0]
print("Loaded %s: %d bytes, %d bytes of working memory" % (MODEL, model.len, model.ram))


def predict(x):
    """The model's guess at sin(x)."""

    # The model asks for its input by handing over a buffer to fill.
    def fill(buf, shape, dtype):
        buf[0] = max(-128, min(127, round(x / in_scale) + in_zero)) & 0xFF

    # The output is one row of one number, in the model's units.
    output = model.predict([fill])[0]
    return (output[0][0] - out_zero) * out_scale


# Ask for a few points across one cycle, and compare with math.sin.
for i in range(POINTS):
    x = 2 * math.pi * i / (POINTS - 1)
    print("sin(%.2f) is %+.3f; the model says %+.3f" % (x, math.sin(x), predict(x)))

# Free the model's memory.
model.deinit()
