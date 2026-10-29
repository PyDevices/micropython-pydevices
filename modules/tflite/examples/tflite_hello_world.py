# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Run TensorFlow Lite Micro's hello-world test through the ``tflite`` module.

Uses the same int8 model, the same four raw inputs and the same 0.05
tolerance against ``math.sin`` as TFLM's own ``hello_world_test.cc``
(``LoadQuantModelAndPerformInference``). The program prints each result and
ends with ``PASS`` or ``FAIL``.

Needs: an ESP32-S3 or ESP32-P4 board with firmware built with tflite, and the
model, ``hello_world_int8.tflite``, from
``tensorflow/lite/micro/examples/hello_world/models/`` in tflite-micro,
copied to the board:

    mpftp put hello_world_int8.tflite /hello_world_int8.tflite

Run it:

    mpftp run tflite_hello_world.py
"""

import math

import tflite

# Load the model.
model = tflite.Model("/hello_world_int8.tflite")
out_scale, out_zero = model.output_scale[0], model.output_zero_point[0]

# TFLM's test inputs, as floats and as the int8 values the model takes.
golden_float = (0.77, 1.57, 2.3, 3.14)
golden_int8 = (-96, -63, -34, 0)
epsilon = 0.05

# Run each input and check the output is within the tolerance of sin(x).
ok = True
for xf, xq in zip(golden_float, golden_int8):

    def fill(buf, shape, dtype, q=xq):
        buf[0] = q & 0xFF

    out = model.predict([fill])[0]
    try:
        raw = out.reshape((1,))[0]
    except (AttributeError, ValueError):
        raw = out[0]
    y = (raw - out_zero) * out_scale
    err = abs(math.sin(xf) - y)
    ok = ok and err <= epsilon
    print("x=%.2f int8 %4d -> y=%.4f sin=%.4f err=%.4f" % (xf, xq, y, math.sin(xf), err))
print("PASS" if ok else "FAIL", "(TFLM's tolerance %.2f)" % epsilon)
model.deinit()
