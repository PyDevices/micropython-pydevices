# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Listen for a wake word in a WAV file.

Runs one of ESPHome's microWakeWord models, such as "Hey Jarvis", over a
recording and prints the time of each detection. audiofeatures turns the
audio into 10 ms feature slices, the tflite model turns every few slices into
a probability, and the detector in this file decides, following ESPHome's
micro_wake_word: the mean of the last few probabilities must pass the model's
cutoff, nothing fires in the first second, and after a detection the
probabilities must fall back below the cutoff before the next.

Needs: an ESP32-S3 or ESP32-P4 board with firmware built with tflite, ulab
and audiodsp, a model and its JSON manifest from ESPHome's
micro-wake-word-models, and a 16-bit PCM WAV file at 16 kHz or higher:

    mpftp put hey_jarvis.tflite /hey_jarvis.tflite
    mpftp put hey_jarvis.json /hey_jarvis.json
    mpftp put speech.wav /speech.wav

For a microphone, give the Frontend an audiopump tap with ``attach()``
instead of feeding it the file.

Run it:

    mpftp run tflite_wakeword.py

Tested on: an ESP32-P4 board on MicroPython v1.30.0-preview.160.gb70ae1aca6
(built 2026-10-29).
"""

import json
import struct

import audiofeatures
import tflite

# Change these to match your setup.
MANIFEST = "/hey_jarvis.json"  # the model's .tflite sits beside it
FILENAME = "/speech.wav"
BLOCK = 480  # frames read and fed at a time
WARM_UP = 100  # slices ignored at the start and after a detection, as ESPHome


def main():
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

        frontend = audiofeatures.Frontend()
        features = bytearray(size * 10)
        probabilities = [0] * window
        ignore = -WARM_UP
        filled, slices, newest, left = 0, 0, 0, length
        while left > 0:
            block = f.read(min(left, BLOCK * 2 * channels))
            left -= len(block)
            frontend.feed(block, rate, channels)
            count = frontend.readinto(features)
            for i in range(count):
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

    print("%d slices, %.1f s of audio" % (slices, slices / 100))


if __name__ == "__main__":
    main()
