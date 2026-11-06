# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Show what's playing over Bluetooth, with a level meter.

A Bluetooth speaker's screen: the title and artist your phone or computer
sends, whether it's playing, and a left and right level meter. When the
board also has a speaker or I2S amplifier, the audio plays through it.

Needs: a classic ESP32 board with a display, firmware built with btaudio,
pygraphics and the board's packages (--modules all,btaudio), and a
board_config with a display_drv. A pcm_out role in board_peripherals adds
the sound.

Run it:

    mpftp run btaudio_speaker.py

Tested on: not yet run.
"""

import math
import time

import btaudio
import network
from board_config import display_drv
from displaydev import color565
from pygraphics import RGB565, FrameBuffer

try:
    import board_peripherals
    from audiodev import AudioFormat
except ImportError:
    board_peripherals = None

# Change these to match your setup.
FRAMES = 512  # frames per write when there's a speaker
FLOOR_DB = -48  # the quietest level the meter shows

BACKGROUND = color565(0, 0, 32)
TEXT = color565(255, 255, 255)
DIM = color565(140, 140, 160)
METER = color565(0, 200, 80)
LOUD = color565(255, 170, 0)

receiver = btaudio.Receiver()
width, height = display_drv.width, display_drv.height
display_drv.fill(BACKGROUND)

# A strip of the screen for the text, drawn with pygraphics and copied on.
TEXT_HEIGHT = 56
pixels = bytearray(width * TEXT_HEIGHT * 2)
strip = FrameBuffer(pixels, width, TEXT_HEIGHT, RGB565)


def draw_text():
    strip.fill(BACKGROUND)
    if not receiver.connected:
        strip.text("Pair with %s" % network.hostname(), 8, 8, TEXT)
    else:
        strip.text(receiver.title or "(no title)", 8, 8, TEXT)
        strip.text(receiver.artist, 8, 24, DIM)
        strip.text("playing" if receiver.playing else "paused", 8, 40, DIM)
    display_drv.blit_rect(pixels, 0, 0, width, TEXT_HEIGHT)


# Each meter bar is the channel's peak in decibels, FLOOR_DB to 0.
BAR_HEIGHT = 20
bar_top = (TEXT_HEIGHT + 8, TEXT_HEIGHT + 8 + BAR_HEIGHT + 6)


def draw_meter():
    for top, peak in zip(bar_top, receiver.levels()):
        db = 20 * math.log10(peak / 32767) if peak else FLOOR_DB
        fill = int(width * max(0, db - FLOOR_DB) / -FLOOR_DB)
        display_drv.fill_rect(0, top, fill, BAR_HEIGHT, LOUD if db > -3 else METER)
        display_drv.fill_rect(fill, top, width - fill, BAR_HEIGHT, BACKGROUND)


# Play through the board's speaker too, if it has one.
speaker = None
rate = 0
buf = bytearray(FRAMES * 4)
pcm_out = getattr(board_peripherals, "pcm_out", None)

draw_text()
display_drv.show()
last = time.ticks_ms()
while True:
    if pcm_out and receiver.sample_rate and receiver.sample_rate != rate:
        rate = receiver.sample_rate
        if speaker:
            speaker.close()
        speaker = pcm_out(AudioFormat(rate, 2, 16))
    if speaker:
        receiver.readinto(buf)
        speaker.write(buf)
    else:
        time.sleep_ms(20)

    # Redraw the text when what it shows changes, and the meter 20 times a second.
    if receiver.changes() & (btaudio.CONNECTION | btaudio.TRACK | btaudio.PLAYING):
        draw_text()
    if time.ticks_diff(time.ticks_ms(), last) >= 50:
        last = time.ticks_ms()
        draw_meter()
        display_drv.show()
