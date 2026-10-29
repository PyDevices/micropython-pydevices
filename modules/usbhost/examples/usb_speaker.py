# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Play a tone through a USB speaker on the host port.

``usb.host.audio.Speaker`` opens the stream, and you write PCM to it a whole
frame at a time. A commercial USB audio interface, a headset, or a board
running ``usb_soundcard.py`` all look the same. Success is a 440 Hz tone from
the speaker. A computer hosting that board works too, because standard
classes are the protocol.

Needs: a board with USB host support (firmware built with usbhost) and a USB
speaker. An ESP32-S3 needs a powered hub or OTG adapter, and two self-powered
boards can back-feed VBUS unless the cable omits it; ``README.md`` in this
folder explains. Hosted playback competes with hosted video for FIFO space,
and the split is fixed when the host starts. This program asks for ``uac``
alone with ``usb.core.start(classes=("uac",))``, which allows a periodic OUT
packet of 200 bytes on an S3 and 800 on a P4. Those fit a USB Audio 2.0
stream of 48 kHz stereo (196 bytes) and a USB-C DAC whose endpoint declares
384. The host prints the split as it starts. A session that also asks for
``uvc`` leans toward IN and can't open such a stream.

Run it:

    mpftp run examples/usb_speaker.py

Tested on: an ESP32-S3 hosting an ESP32-P4 panel that was running
``usb_soundcard.py``.
"""

import math
import time

import usb.core
from usb.host.audio import Speaker


def find_speaker(timeout_ms=15000):
    deadline = time.ticks_add(time.ticks_ms(), timeout_ms)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        dev = usb.core.find(custom_match=Speaker.match)
        if dev is not None:
            return dev
        time.sleep_ms(250)
    return None


def tone_frames(rate, channels, bits, freq=440.0, seconds=2.0, volume=0.2):
    """Generate a sine as int16 little-endian frames."""
    n = int(rate * seconds)
    amp = int(32767 * volume)
    # One frame = channels samples.
    raw = bytearray(n * channels * (bits // 8))
    for i in range(n):
        sample = int(amp * math.sin(2 * math.pi * freq * i / rate))
        # int16 LE
        lo = sample & 0xFF
        hi = (sample >> 8) & 0xFF
        base = i * channels * 2
        for ch in range(channels):
            raw[base + ch * 2] = lo
            raw[base + ch * 2 + 1] = hi
    return raw


def main():
    usb.core.start(classes=("uac",))
    dev = find_speaker()
    if dev is None:
        print("no USB audio output found")
        print("attach a speaker, headset, or a board running usb_soundcard.py")
        usb.core.stop()
        return

    print("audio device", dev, dev.manufacturer, dev.product)

    # The tone below is 16-bit, so ask for a 16-bit stream; the rate and
    # channels are the device's best offer.
    out = Speaker(dev, bits=16)
    s = out.stream
    print("  ep 0x%02x, %d B packets" % (s.endpoint, s.max_packet))
    rate, channels, bits = out.rate, out.channels, out.bits
    print("playing 440 Hz for 2 s at %d Hz %d ch %d-bit" % (rate, channels, bits))

    pcm = tone_frames(rate, channels, bits)
    view = memoryview(pcm)
    sent = 0
    out.start()
    try:
        while sent < len(pcm):
            n = out.write(view[sent:])
            if n <= 0:
                time.sleep_ms(5)
                continue
            sent += n
        # Let the ring drain so the tail of the tone is audible.
        time.sleep_ms(500)
    finally:
        print("sent", sent, "bytes; stats", out.stats())
        out.stop()
        usb.core.stop()


if __name__ == "__main__":
    main()
