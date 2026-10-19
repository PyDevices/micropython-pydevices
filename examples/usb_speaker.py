"""Play a tone through a hosted USB speaker -- the S3 half of the sound card.

A commercial USB audio interface, a headset, or a PyDevices board running
``usb_soundcard.py`` all look the same here: ``usb.host.audio.Speaker`` opens
the stream and you write PCM to it, a whole frame at a time.

    mpftp run -d COM49 examples/usb_speaker.py

**Headline pairing.** P4 runs ``usb_soundcard.py`` (device, audiodsp's pump into its ES8311).
This script on an S3 hosts that P4 and plays through it. A PC hosting the P4
works too -- standard classes are the protocol.

**Power.** S3 host needs a powered hub or OTG adapter. Two self-powered boards
can back-feed unless the cable omits VBUS; see ``examples/README.md``.

**FIFO split.** Hosted playback competes with hosted video for DWC FIFO
space, and the split is fixed when the host starts (usbif#2). The host picks
it from the classes it is started for: this script asks for ``uac`` alone
(``usb.core.start(classes=("uac",))``),
which gives periodic OUT 200 bytes on an S3, enough for 48 kHz stereo from a
USB Audio 2.0 device (196), and 800 on a P4, enough for a USB-C DAC whose
endpoint declares 384. The host prints the split and those limits as it
starts. A session that also asks for ``uvc`` leans IN and
cannot open such a stream.
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
