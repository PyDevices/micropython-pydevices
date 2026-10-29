# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Name the sounds in a WAV file and score it for a crying baby.

Runs yamnet-cry-distill-int8, a 110 KB student of Google's YAMNet that
answers with the same 521 AudioSet classes (speech, a doorbell, a smoke
alarm, a dog, a baby crying), over a recording. The program turns the audio
into YAMNet's input itself, with ulab: 0.96 s patches of 96 log-mel frames
of 64 bands, every 0.48 s. It prints the three likeliest sounds in each
patch, the five likeliest over the whole file, and the crying score the
model's authors recommend, with its threshold.

Needs: an ESP32-S3 or ESP32-P4 board with firmware built with tflite and
ulab, and a 16-bit PCM WAV file of mono audio at 16 kHz or higher. Download
model.tflite from huggingface.co/chayuto/yamnet-cry-distill-int8 and
yamnet_class_map.csv from Google's research/audioset/yamnet folder in
github.com/tensorflow/models, then copy them over:

    mpftp put model.tflite /yamnet_cry.tflite
    mpftp put yamnet_class_map.csv /yamnet_class_map.csv
    mpftp put alarm.wav /alarm.wav

Run it:

    mpftp run tflite_sounds.py

Tested on: an ESP32-P4 board on MicroPython v1.30.0-preview.160.gb70ae1aca6
(built 2026-10-29).
"""

import math
import struct
import time

import tflite
from ulab import numpy as np

# Change these to match your setup.
MODEL = "/yamnet_cry.tflite"
CLASSES = "/yamnet_class_map.csv"
FILENAME = "/alarm.wav"
CRY_THRESHOLD = 0.047  # the model card's best operating point

# YAMNet's front end: 16 kHz, a 25 ms window every 10 ms, 64 mel bands
# from 125 Hz to 7.5 kHz, and patches of 96 frames every 48.
RATE, WINDOW, HOP, FFT, BANDS, FRAMES = 16000, 400, 160, 512, 64, 96


def read_wav(path):
    # Read a mono 16-bit WAV into floats from -1 to 1, at 16 kHz.
    with open(path, "rb") as f:
        assert f.read(12)[8:] == b"WAVE", "not a WAV file"
        while True:
            tag, length = struct.unpack("<4sI", f.read(8))
            if tag == b"data":
                break
            body = f.read(length + (length & 1))
            if tag == b"fmt ":
                kind, channels, rate = struct.unpack("<HHI", body[:8])
                assert kind == 1 and channels == 1 and body[14] == 16, "16-bit mono PCM only"
        samples = np.frombuffer(f.read(length), dtype=np.int16)
    audio = np.array(samples, dtype=np.float) / 32768
    if rate != RATE:
        # Resample by linear interpolation.
        count = len(audio) * RATE // rate
        audio = np.interp(np.arange(count) * (rate / RATE), np.arange(len(audio)), audio)
    return audio


def mel_matrix():
    # Triangular filters on the HTK mel scale, one column per band.
    def mel(hz):
        return 1127 * math.log(1 + hz / 700)

    low, high = mel(125), mel(7500)
    edges = [low + (high - low) * i / (BANDS + 1) for i in range(BANDS + 2)]
    weights = np.zeros((FFT // 2 + 1, BANDS))
    for k in range(1, FFT // 2 + 1):
        m = mel(k * RATE / FFT)
        for b in range(BANDS):
            left, centre, right = edges[b], edges[b + 1], edges[b + 2]
            if left < m < right:
                if m <= centre:
                    weights[k, b] = (m - left) / (centre - left)
                else:
                    weights[k, b] = (right - m) / (right - centre)
    return weights


def log_mel(audio):
    # One row of 64 log-mel values for every 10 ms of audio.
    weights = mel_matrix()
    hann = 0.5 - 0.5 * np.cos(2 * math.pi * np.arange(WINDOW) / WINDOW)
    frame = np.zeros(FFT)
    count = 1 + (len(audio) - WINDOW) // HOP
    rows = np.zeros((count, BANDS))
    for i in range(count):
        frame[:WINDOW] = audio[i * HOP : i * HOP + WINDOW] * hann
        real, imag = np.fft.fft(frame)
        magnitude = np.sqrt(real[: FFT // 2 + 1] ** 2 + imag[: FFT // 2 + 1] ** 2)
        rows[i, :] = np.log(np.dot(magnitude, weights) + 0.001)
    return rows


# Load the model and the class names.
model = tflite.Model(MODEL)
print("model:", model.len, "bytes, arena", model.ram, "bytes")
with open(CLASSES) as f:
    f.readline()
    names = [line.strip().split(",", 2)[2].strip('"') for line in f]
scale, zero = model.input_scale[0], model.input_zero_point[0]
out_scale, out_zero = model.output_scale[0], model.output_zero_point[0]

# Turn the recording into log-mel rows.
t0 = time.ticks_ms()
rows = log_mel(read_wav(FILENAME))
print("%d frames in %d ms" % (len(rows), time.ticks_diff(time.ticks_ms(), t0)))

# Run the model on each patch. Its answers are logits, so a softmax turns
# them into probabilities that add up to 1.
total = np.zeros(len(names))
patches = 0
spent = 0
for start in range(0, len(rows) - FRAMES + 1, FRAMES // 2):
    patch = np.clip(np.around(rows[start : start + FRAMES, :] / scale) + zero, -128, 127)
    data = np.array(patch.flatten(), dtype=np.int8).tobytes()

    def fill(buffer, shape, dtype):
        buffer[:] = data

    t0 = time.ticks_us()
    logits = (np.array(model.predict([fill])[0][0], dtype=np.float) - out_zero) * out_scale
    spent += time.ticks_diff(time.ticks_us(), t0)
    odds = np.exp(logits - np.max(logits))
    odds = odds / np.sum(odds)
    total += odds
    patches += 1
    best = np.argsort(-odds, axis=0)[:3]
    print("%5.2f s:" % (start * HOP / RATE), ", ".join("%s %.2f" % (names[k], odds[k]) for k in best))

# Average the patches and print the overall answer.
total = total / patches
best = np.argsort(-total, axis=0)[:5]
print("whole file:", ", ".join("%s %.2f" % (names[k], total[k]) for k in best))
cry = total[19] + total[20]
print("crying score %.3f (%s)" % (cry, "crying" if cry > CRY_THRESHOLD else "not crying"))
print("%d patches, %d ms per inference" % (patches, spent // patches // 1000))
model.deinit()
