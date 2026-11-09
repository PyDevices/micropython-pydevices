# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Run ESP-DL detection in a thread while the main loop keeps going.

Starts a thread that runs the pedestrian detector on a photo ten times,
while the main thread does nothing but count its own loop and note the
longest pause between two turns. espdl lets go of the interpreter while a
model runs, so the main loop keeps turning during each detection: the
longest pause comes out well under the time one detection takes. A
display, a UI or a recording loop on the main thread gets the same.

Needs: an ESP32-S3 or ESP32-P4 board with firmware built with vision, and
the photo and model espdl_pedestrian_test.py uses, copied to the board:

    mpftp put pedestrian.jpg /pedestrian.jpg
    mpftp put pedestrian_detect_pico.espdl /pedestrian.espdl

Run it:

    mpftp run espdl_thread.py
"""

import _thread
import time

import espdl
import image

# Change these to match your setup.
PHOTO = "/pedestrian.jpg"
MODEL = "/pedestrian.espdl"
RUNS = 10
THREAD_STACK = 32 * 1024  # bytes; the model runs on the thread's own stack

# Load the photo as RGB565, which the detector takes, and the model.
img = image.Image(PHOTO).to_rgb565(copy=True)
detector = espdl.PedestrianDetect(MODEL)
results = []


def detect_loop():
    # Detect RUNS times, then hand back the last count and the total time.
    start = time.ticks_ms()
    found = []
    for _ in range(RUNS):
        found = detector.detect(img)
    results.append((len(found), time.ticks_diff(time.ticks_ms(), start)))


# Start the detector on its own thread.
_thread.stack_size(THREAD_STACK)
_thread.start_new_thread(detect_loop, ())

# Count turns of the main loop until the thread is done, and keep the
# longest gap between two turns.
turns = 0
longest_us = 0
last = time.ticks_us()
while not results:
    now = time.ticks_us()
    longest_us = max(longest_us, time.ticks_diff(now, last))
    last = now
    turns += 1

# Report the main loop's turns beside what the detector did meanwhile.
count, total_ms = results[0]
print("%d detections, %d ms each, %d people in the photo" % (RUNS, total_ms // RUNS, count))
print("main loop: %d turns meanwhile, longest pause %.1f ms" % (turns, longest_us / 1000))
detector.deinit()
