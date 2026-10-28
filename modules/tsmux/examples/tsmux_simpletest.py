# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Cut live video into HLS segments that any player can stream.

Encodes four seconds of a white square crossing a blue background with
h264enc, and hands each frame to ``tsmux.Segmenter``, which packs them
into one-second MPEG-TS segments and keeps a playlist of them. At the
end it writes the playlist and the segments it finished to the filesystem:
three of them, since the last second has no keyframe after it to close it.
Copy them to your computer and play the playlist with ffplay or VLC.

A real stream serves the same playlist and segments over HTTP while the
encoder runs; the Segmenter keeps the newest ones for that.

Needs: an ESP32-P4 board, and firmware built with h264enc, tsmux and ppa
(``--modules all`` has all three).

Run it:

    mpremote run tsmux_simpletest.py
    mpremote cp :stream.m3u8 :seg*.ts .
    ffplay stream.m3u8

Tested on: not yet run.
"""

import h264enc
import tsmux

# Change these to match your setup.
WIDTH = 320  # a multiple of 16
HEIGHT = 240  # a multiple of 16
FPS = 30
SECONDS = 4
BOX = 48

# RGB565 colours, in the board's byte order.
BLUE = b"\x1f\x00"
WHITE = b"\xff\xff"

# Make the background once, and a frame buffer to draw into.
background = BLUE * (WIDTH * HEIGHT)
frame = bytearray(background)
box_row = WHITE * BOX

# A keyframe every second, so every one-second segment can start with one.
encoder = h264enc.Encoder(WIDTH, HEIGHT, fps=FPS, gop=FPS)
segmenter = tsmux.Segmenter(segments=3, target=1000)

frames = FPS * SECONDS
for n in range(frames):
    # Draw the frame: background first, then the square.
    frame[:] = background
    x = n * (WIDTH - BOX) // frames
    y = (HEIGHT - BOX) // 2
    for row in range(y, y + BOX):
        start = (row * WIDTH + x) * 2
        frame[start : start + BOX * 2] = box_row

    # Encode it and add it to the stream. Timestamps count 90 kHz ticks.
    access_unit = encoder.encode(frame)
    if segmenter.add(access_unit, n * 90000 // FPS, encoder.keyframe):
        print("Finished a segment; holding %d" % segmenter.count)

encoder.close()

# Write the playlist and each segment it lists.
with open("stream.m3u8", "w") as f:
    f.write(segmenter.playlist())
for n in range(segmenter.first, segmenter.first + segmenter.count):
    with open("seg%d.ts" % n, "wb") as f:
        f.write(segmenter.segment(n))
print("Wrote stream.m3u8 and %d segments" % segmenter.count)
