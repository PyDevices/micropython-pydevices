# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Compare a TensorFlow Lite model with ``math.sin``.

Runs esp-vision's sine model (``models/tflite/sine/sine.tflite`` in esp-vision)
over one cycle and compares it with ``math.sin``. It is the smallest proof that
the ``tflite`` module loads a model, fills its input and reads its output on
this board. The program prints the model's tensors, the worst error and where
it happened, and the time per prediction. The model is small and approximate:
its own worst error is about 0.12, the same on the P4 and the S3.
``tflm_hello_world_test.py`` is the pass or fail test.

Needs: an ESP32-S3 or ESP32-P4 board with firmware built with tflite, and the
model on the board:

    mpftp put sine.tflite /sine.tflite

Run it:

    mpftp run sine_check.py
"""

import math
import struct
import time

import tflite

# Change these to match your setup.
MODEL = "/sine.tflite"
STEPS = 64  # points to test over one cycle

# Load the model and print its tensors.
model = tflite.Model(MODEL)
print("model:", model.len, "bytes, arena", model.ram, "bytes")
print("input:", model.input_shape, model.input_dtype, model.input_scale, model.input_zero_point)
print("output:", model.output_shape, model.output_dtype, model.output_scale, model.output_zero_point)

# The quantisation of the input and output, for int8 models.
in_scale, in_zero = model.input_scale[0], model.input_zero_point[0]
out_scale, out_zero = model.output_scale[0], model.output_zero_point[0]


def filler(x):
    # Write one scalar into the input tensor, quantised if the model is int8.
    def fill(buf, shape, dtype):
        if dtype == ord("f"):
            struct.pack_into("<f", buf, 0, x)
        elif dtype == ord("b"):
            buf[0] = max(-128, min(127, round(x / in_scale) + in_zero)) & 0xFF
        elif dtype == ord("B"):
            buf[0] = max(0, min(255, round(x / in_scale) + in_zero))
        else:
            raise ValueError("unexpected input dtype %r" % dtype)

    return fill


def predict(x):
    # Run the model on x and turn the output back into a number.
    out = model.predict([filler(x)])[0]
    try:
        v = out.reshape((1,))[0]
    except (AttributeError, ValueError):
        v = out[0]
    return (v - out_zero) * out_scale if out_scale else v


# Compare the model with math.sin at evenly spaced points over one cycle.
worst, worst_x = 0.0, 0.0
t0 = time.ticks_us()
for i in range(STEPS):
    x = 2 * math.pi * i / (STEPS - 1)
    err = abs(predict(x) - math.sin(x))
    if err > worst:
        worst, worst_x = err, x
dt = time.ticks_diff(time.ticks_us(), t0) / STEPS
print("worst error %.3f at x=%.2f over %d points; %.0f us per prediction" % (worst, worst_x, STEPS, dt))
model.deinit()
