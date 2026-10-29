# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Capture a few seconds from a USB microphone on the host port.

The board hosts a commercial USB microphone, or any USB Audio Class capture
device, through ``usb.host.audio.Microphone``: start it, then read PCM out a
whole frame at a time. The program prints the peak and RMS over the captured
buffer, so you can see that real audio arrived and not silence. A good run
over a speaking voice reports no dropped packets.

Needs: a board with USB host support (firmware built with usbhost) and a USB
microphone. An ESP32-S3 needs a powered hub or OTG adapter. Behind a hub on
an ESP32-P4, set ``FULL_SPEED = True`` below: a USB microphone is a
full-speed device, and the P4's high-speed port can't reach one through a hub
without a transaction translator, which the ESP-IDF host lacks. At full speed
the hub passes it straight through.

Run it, and speak into the microphone:

    mpftp run examples/usb_mic.py

Tested on: a C-Media USB microphone.
"""

import time

import usb.core
from usb.host.audio import Microphone

# Change these to match your setup.
SECONDS = 3

# True when the mic is behind a hub on an ESP32-P4: the host then runs at full
# speed, so the hub passes a full-speed device through. Plugged straight into
# the port, or on an S3, leave it False.
FULL_SPEED = False


def find_mic(timeout_ms=15000):
    deadline = time.ticks_add(time.ticks_ms(), timeout_ms)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        dev = usb.core.find(custom_match=Microphone.match)
        if dev is not None:
            return dev
        time.sleep_ms(250)
    return None


def _peak_rms(buf, sample_bytes=2):
    """Peak absolute sample and RMS over int16 LE mono/stereo interleaved."""
    n = len(buf) // sample_bytes
    if n == 0:
        return 0, 0.0
    peak = 0
    acc = 0
    for i in range(0, len(buf), sample_bytes):
        sample = buf[i] | (buf[i + 1] << 8)
        if sample >= 0x8000:
            sample -= 0x10000
        a = sample if sample >= 0 else -sample
        if a > peak:
            peak = a
        acc += sample * sample
    rms = (acc / n) ** 0.5
    return peak, rms


def main():
    usb.core.start(full_speed=FULL_SPEED, classes=("uac",))
    dev = find_mic()
    if dev is None:
        print("no USB audio input found")
        print("attach a USB microphone")
        print("note: a PyDevices board cannot present as a mic yet (usbif#7)")
        usb.core.stop()
        return

    print("audio device", dev, dev.manufacturer, dev.product)

    # The peak and RMS below read 16-bit samples, so ask for a 16-bit
    # stream; the rate and channels are the device's best offer.
    mic = Microphone(dev, bits=16)
    s = mic.stream
    print("  ep 0x%02x, %d B packets" % (s.endpoint, s.max_packet))
    rate, channels, bits = mic.rate, mic.channels, mic.bits
    print("capturing %d s at %d Hz %d ch %d-bit" % (SECONDS, rate, channels, bits))

    frame = mic.frame_bytes
    want = int(rate * SECONDS) * frame
    buf = bytearray(want)
    view = memoryview(buf)
    got = 0
    mic.start()
    t0 = time.ticks_ms()
    try:
        while got < want and time.ticks_diff(time.ticks_ms(), t0) < (SECONDS + 2) * 1000:
            n = mic.read(view[got:])
            if n <= 0:
                time.sleep_ms(5)
                continue
            got += n
    finally:
        peak, rms = _peak_rms(buf[:got])
        print("got %d / %d bytes; peak=%d rms=%.1f; stats %r"
              % (got, want, peak, rms, mic.stats()))
        mic.stop()
        usb.core.stop()


if __name__ == "__main__":
    main()
