# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Listen for a wake word on the board's microphone.

The live version of ``tflite_wakeword.py``: it runs one of ESPHome's
microWakeWord models, such as "Hey Jarvis", on what the board's microphone
hears, and prints each detection as it happens. The microphone's samples go
to audiofeatures, which turns them into 10 ms feature slices, and the model
turns every few slices into a probability. The detector follows ESPHome's
micro_wake_word: the mean of the last few probabilities must pass the
model's cutoff, nothing fires in the first second, and after a detection the
probabilities must fall back below the cutoff before the next.

Needs: an ESP32-S3 or ESP32-P4 board with a microphone set up in
board_peripherals (``pcm_in``), firmware built with tflite, ulab and
audiodsp, and a model with its JSON manifest from ESPHome's
micro-wake-word-models:

    mpftp put hey_jarvis.tflite /hey_jarvis.tflite
    mpftp put hey_jarvis.json /hey_jarvis.json

Run it, then say the wake word:

    mpftp run tflite_wakeword_mic.py

Tested on: a Waveshare ESP32-P4-WIFI6-Touch-LCD-4B on MicroPython
v1.30.0-preview.160.gb70ae1aca6 (built 2026-10-29), with its ES7210
microphones.
"""

import json
import time

import audiofeatures
import board_peripherals
import tflite

# Change these to match your setup.
MANIFEST = "/hey_jarvis.json"  # the model's .tflite sits beside it
SECONDS = 60  # how long to listen
WARM_UP = 100  # slices ignored at the start and after a detection, as ESPHome

# Load the model its manifest names, with the manifest's settings.
with open(MANIFEST) as f:
    manifest = json.load(f)
micro = manifest["micro"]
folder = MANIFEST[: MANIFEST.rfind("/") + 1]
model = tflite.Model(folder + manifest["model"])
cutoff = int(micro["probability_cutoff"] * 255)
window = micro["sliding_window_size"]
stride = model.input_shape[0][1]
size = audiofeatures.FEATURE_SIZE

# Slices go straight into the model's int8 input tensor.
group = bytearray(stride * size)


def fill(buffer, shape, dtype):
    buffer[:] = group


# Open the microphone. audiofeatures resamples whatever rate it gives.
microphone = board_peripherals.pcm_in()
microphone.open()
rate = microphone.format.rate
frontend = audiofeatures.Frontend()
block = bytearray(rate // 50 * 2)  # 20 ms of mono 16-bit samples
features = bytearray(size * 10)
probabilities = [0] * window
ignore = -WARM_UP
filled, slices, newest = 0, 0, 0
print("listening for %r for %d s" % (manifest["wake_word"], SECONDS))

start = time.ticks_ms()
try:
    while time.ticks_diff(time.ticks_ms(), start) < SECONDS * 1000:
        # Read what the microphone has and turn it into slices.
        count = microphone.readinto(block)
        if not count:
            time.sleep_ms(2)
            continue
        frontend.feed(memoryview(block)[:count], rate, 1)
        for i in range(frontend.readinto(features)):
            # Gather stride slices, then run the model on them.
            group[filled * size : (filled + 1) * size] = features[i * size : (i + 1) * size]
            filled += 1
            slices += 1
            fresh = filled == stride
            if fresh:
                filled = 0
                newest = model.predict([fill])[0][0][0]
                probabilities = probabilities[1:] + [newest]

            # After a detection, count only the slices below the cutoff.
            if newest < cutoff:
                ignore = min(ignore + 1, 0)

            # Fire when the window's mean passes the cutoff.
            if fresh and ignore >= 0 and sum(probabilities) > cutoff * window:
                print("%s at %.2f s" % (manifest["wake_word"], slices / 100))
                probabilities = [0] * window
                ignore = -WARM_UP
finally:
    # Release the microphone for the next program.
    microphone.close()

print("%d slices, %d dropped" % (slices, frontend.stats()["dropped"]))
