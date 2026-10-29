# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Hear "yes" and "no" in a WAV file.

Runs TensorFlow Lite Micro's micro_speech model, which sorts one second of
speech into silence, unknown, yes or no. audiofeatures makes the model's
input: it is the same audio frontend that micro_speech's own int8 audio
preprocessor model is built from, with a 30 ms window and 40 channels. The
model wants a slice every 20 ms and audiofeatures makes one every 10 ms, so
the program keeps every second slice. It slides a one-second window over the
recording in steps of 200 ms and prints each window's answer, then the most
confident word heard.

Needs: an ESP32-S3 or ESP32-P4 board with firmware built with tflite and
audiodsp, micro_speech_quantized.tflite from tflite-micro's
tensorflow/lite/micro/examples/micro_speech/models folder, and a 16-bit PCM
WAV file at 16 kHz or higher, such as yes_1000ms.wav from that example's
testdata folder:

    mpftp put micro_speech_quantized.tflite /micro_speech.tflite
    mpftp put yes_1000ms.wav /yes.wav

Run it:

    mpftp run tflite_micro_speech.py

Tested on: an ESP32-P4 board on MicroPython v1.30.0-preview.160.gb70ae1aca6
(built 2026-10-29).
"""

import struct
import time

import audiofeatures
import tflite

# Change these to match your setup.
MODEL = "/micro_speech.tflite"
FILENAME = "/yes.wav"
STEP = 10  # model slices (20 ms each) between windows
LABELS = ("silence", "unknown", "yes", "no")

# Load the model. Its input is 49 slices of 40 values, one second.
model = tflite.Model(MODEL)
print("model:", model.len, "bytes, arena", model.ram, "bytes")
size = audiofeatures.FEATURE_SIZE
count = model.input_shape[0][1] // size
out_scale, out_zero = model.output_scale[0], model.output_zero_point[0]

# Turn the whole recording into feature slices, keeping every second one.
frontend = audiofeatures.Frontend(slices=1000)
kept = bytearray()
with open(FILENAME, "rb") as f:
    # Walk the WAV's chunks to its format and the start of its samples.
    assert f.read(12)[8:] == b"WAVE", "not a WAV file"
    while True:
        tag, length = struct.unpack("<4sI", f.read(8))
        if tag == b"data":
            break
        body = f.read(length + (length & 1))
        if tag == b"fmt ":
            kind, channels, rate = struct.unpack("<HHI", body[:8])
            assert kind == 1 and body[14] == 16, "16-bit PCM only"
    features = bytearray(size * 100)
    taken = 0
    while length > 0:
        block = f.read(min(length, 3200 * channels))
        length -= len(block)
        frontend.feed(block, rate, channels)
        n = frontend.readinto(features)
        for i in range(n):
            if taken % 2 == 0:
                kept += features[i * size : (i + 1) * size]
            taken += 1

# Pad a short recording with silent slices so there is at least one window.
slices = len(kept) // size
if slices < count:
    kept += bytes([0x80]) * ((count - slices) * size)
    slices = count

# Run the model on each one-second window.
best = (0.0, "silence", 0)
for start in range(0, slices - count + 1, STEP):
    window = kept[start * size : (start + count) * size]

    def fill(buffer, shape, dtype):
        buffer[:] = window

    t0 = time.ticks_us()
    scores = model.predict([fill])[0][0]
    took = time.ticks_diff(time.ticks_us(), t0)
    odds = [(scores[i] - out_zero) * out_scale for i in range(len(LABELS))]
    top = odds.index(max(odds))
    print("%4.1f s: %-7s %s  (%d us)" % (start * 0.02, LABELS[top], " ".join("%.2f" % p for p in odds), took))
    if top >= 2 and odds[top] > best[0]:
        best = (odds[top], LABELS[top], start)

if best[0]:
    print("heard %s at %.1f s (%.2f)" % (best[1], best[2] * 0.02, best[0]))
else:
    print("heard neither yes nor no")
model.deinit()
