# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Find faces in a photo and mark their eyes, nose and mouth.

Runs ESP-DL's two-stage face detector (HumanFaceDetect) on a photo. Each
face comes back as a box, a score and five key points: the eyes, the tip
of the nose and the corners of the mouth. The program prints them, draws
the boxes and points on the photo and saves it as a JPEG for you to look at.

Needs: an ESP32-S3 or ESP32-P4 board with firmware built with vision, a
photo with a face in it, and the two models of ESP-DL's human_face_detect
(MIT licence; the files for your chip, from its models/p4 or models/s3
folder), copied to the board:

    mpftp put face.jpg /face.jpg
    mpftp put human_face_detect_msr_s8_v1.espdl /models/human_face_detect_msr_s8_v1.espdl
    mpftp put human_face_detect_mnp_s8_v1.espdl /models/human_face_detect_mnp_s8_v1.espdl

Run it:

    mpftp run espdl_face_landmarks.py
    mpftp get /faces.jpg faces.jpg
"""

import time

import espdl
import image

# Change these to match your setup.
PHOTO = "/face.jpg"
MSR_MODEL = "/models/human_face_detect_msr_s8_v1.espdl"
MNP_MODEL = "/models/human_face_detect_mnp_s8_v1.espdl"
FILENAME = "/faces.jpg"

# Load the photo as RGB565, which the detector takes, and both models.
img = image.Image(PHOTO).to_rgb565(copy=True)
detector = espdl.HumanFaceDetect(MSR_MODEL, MNP_MODEL)

# Detect, timing the call.
start = time.ticks_ms()
faces = detector.detect(img)
print("%d faces in %d ms" % (len(faces), time.ticks_diff(time.ticks_ms(), start)))

# Print each face, and draw its box in green and its five points in red.
for x, y, w, h, score, points in faces:
    print("face at (%d, %d), %dx%d, score %.2f" % (x, y, w, h, score))
    print("  points:", [(points[i], points[i + 1]) for i in range(0, len(points), 2)])
    img.draw_rectangle(x, y, w, h, color=(0, 255, 0), thickness=2)
    for i in range(0, len(points), 2):
        img.draw_circle(points[i], points[i + 1], 3, color=(255, 0, 0), fill=True)

# Save the marked photo as a JPEG.
jpeg = img.to_jpeg(quality=85, copy=True)
with open(FILENAME, "wb") as f:
    f.write(jpeg.bytearray())
print("Saved", FILENAME)
detector.deinit()
