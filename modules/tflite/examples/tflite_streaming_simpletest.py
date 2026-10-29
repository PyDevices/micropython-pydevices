# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Run a streaming wake word model on a few seconds of silence.

A streaming model, such as the microWakeWord models ESPHome publishes, sees
a few feature slices at a time and remembers the ones before them between
calls. This program loads one, feeds it the slices of silence, a group of
``stride`` slices per call, and prints the model's output, which should stay
near 0. The input is written straight into the model's tensor by a callable,
since the slices are int8 values ready for the model already.

Needs: an ESP32-S3 or ESP32-P4 board with firmware built with tflite and
ulab, and a microWakeWord model (``hey_jarvis.tflite`` from ESPHome's
micro-wake-word-models) on the board:

    mpftp put hey_jarvis.tflite /hey_jarvis.tflite

Run it:

    mpftp run tflite_streaming_simpletest.py

Tested on: an ESP32-P4 board on MicroPython v1.30.0-preview.160.gb70ae1aca6
(built 2026-10-29).
"""

import tflite

# Change these to match your setup.
MODEL = "/hey_jarvis.tflite"
SLICES = 300  # 10 ms feature slices: 3 seconds
SILENCE = -128  # the int8 feature value of a silent slice

# Load the model. Its input is [1, stride, 40]: stride slices of 40 values.
model = tflite.Model(MODEL)
stride, features = model.input_shape[0][1], model.input_shape[0][2]
print("stride", stride, "slices per call, arena", model.ram, "bytes")

# One call's worth of slices.
group = bytearray([SILENCE & 0xFF] * (stride * features))


def fill(buffer, shape, dtype):
    # Copy the slices into the input tensor as they are.
    buffer[:] = group


# Feed the slices a call at a time. The output is uint8: 255 means certain.
peak = 0
for call in range(SLICES // stride):
    probability = model.predict([fill])[0][0][0]
    peak = max(peak, probability)
print("calls", SLICES // stride, "peak probability", peak / 255)
