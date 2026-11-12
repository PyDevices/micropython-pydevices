# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Record a second from a USB microphone and print its level.

The board is the USB host and the microphone its device: any USB Audio Class
capture device, such as a headset or a sound card's input.
``usb.host.audio.Microphone`` negotiates a 16-bit stream and fills a buffer
with PCM. The program prints the loudest sample it saw, so you can tell real
sound from silence. ``usb_mic.py`` adds the RMS level and the stream's
counters.

Needs: a board with USB host support (firmware built with usbhost), the
``usb-host-audio`` package, and a USB microphone in the host port. Behind a
hub on an ESP32-P4, leave ``FULL_SPEED = True``: a microphone is a full-speed
device, and a high-speed hub can't reach one otherwise.

Run it:

    mpftp run --follow examples/usb_host_audio_simpletest.py

Make some noise near the microphone. Success is a peak well above zero.

Tested on: Waveshare ESP32-P4-WIFI6-DEV-KIT on MicroPython
v1.30.0-preview.160.gb70ae1aca6 (built 2026-11-11), behind its onboard hub, with FULL_SPEED = True, hosting an Arduino GIGA R1 WiFi presenting a USB microphone
(``usb.device.audio``) with a 500 Hz tone: it read one second of 48 kHz stereo and
reported the tone's peak.
"""

import time

import usb.core
from usb.host.audio import Microphone

# Change these to match your setup.
FULL_SPEED = True
SECONDS = 1  # how much to record

# Start the host and wait for a microphone to enumerate.
usb.core.start(full_speed=FULL_SPEED, classes=("uac",))
try:
    device = None
    while device is None:
        time.sleep_ms(250)
        device = usb.core.find(custom_match=Microphone.match)
    mic = Microphone(device, bits=16)
    print("Microphone:", device.product, mic.rate, "Hz,", mic.channels, "channels")

    # Read PCM until the buffer is full, or the time and a margin are up.
    buffer = bytearray(mic.rate * SECONDS * mic.frame_bytes)
    view = memoryview(buffer)
    got = 0
    mic.start()
    start = time.ticks_ms()
    while got < len(buffer) and time.ticks_diff(time.ticks_ms(), start) < (SECONDS + 2) * 1000:
        got += max(mic.read(view[got:]), 0)
    mic.stop()

    # Find the loudest 16-bit sample.
    peak = 0
    for i in range(0, got - 1, 2):
        sample = buffer[i] | buffer[i + 1] << 8
        peak = max(peak, abs(sample - 0x10000 if sample >= 0x8000 else sample))
    print("Read", got, "bytes, peak", peak, "of 32767")
finally:
    # Release the port for the next program.
    usb.core.stop()
