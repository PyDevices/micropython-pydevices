# usb.host.audio on usb.core: play a tone into the hosted speaker.
import math
import time

import usb.core
from usb.host.audio import Speaker


def run(seconds=3, hz=440, amp=0.3):
    usb.core.start(classes=("uac",))
    dev = None
    for _ in range(40):
        dev = usb.core.find(custom_match=Speaker.match)
        if dev:
            break
        time.sleep_ms(250)
    spk = Speaker(dev, rate=48000, channels=2, bits=16, ring_ms=200)
    period = 48000 // hz if 48000 % hz == 0 else 48000 // hz
    one = bytearray(4 * 1200)            # 25 ms of tone, a whole number of 440 Hz cycles near enough
    for i in range(1200):
        v = int(amp * 32767 * math.sin(2 * math.pi * hz * i / 48000))
        one[i * 4:i * 4 + 4] = bytes((v & 0xFF, (v >> 8) & 0xFF, v & 0xFF, (v >> 8) & 0xFF))
    sent = 0
    t0 = time.ticks_ms()
    with spk:
        end = time.ticks_add(t0, seconds * 1000)
        mv = memoryview(one)
        at = 0
        while time.ticks_diff(end, time.ticks_ms()) > 0:
            n = spk.write(mv[at:])
            sent += n
            at = (at + n) % len(one)
            if not n:
                time.sleep_ms(5)
        time.sleep_ms(300)
        stats = spk.stats()
    return {"device": "%s %s" % (dev.manufacturer, dev.product), "stream": (spk.rate, spk.channels, spk.bits),
            "sent": sent, "expected": seconds * 48000 * 4, "stats": stats}
