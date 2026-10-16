# Board half of the sound-card stream check: bring up usb-device-audio's
# speaker beside the console, then drain its ring for a while and report what
# arrived. Driven over a held REPL (tools/stream/speaker_check.py).
import time

import usb.device
from usb.device.audio import SpeakerInterface

spk = None


def start():
    global spk
    spk = SpeakerInterface()
    usb.device.get().init(spk, builtin_driver=True)
    return spk.packet


def run(seconds):
    """Drain the ring for this long; returns what came, and its loudest sample."""
    buf = bytearray(1024)
    mv = memoryview(buf)
    got = 0
    peak = 0
    streamed_ms = 0
    end = time.ticks_add(time.ticks_ms(), int(seconds * 1000))
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        spk.service()
        n = spk.ring.readinto(buf)
        if n:
            got += n
            # Every 64th sample's magnitude, enough to see a tone's level.
            for i in range(0, n - 1, 128):
                v = buf[i] | buf[i + 1] << 8
                if v & 0x8000:
                    v = 0x10000 - v
                if v > peak:
                    peak = v
        if spk.streaming:
            streamed_ms += 2
        time.sleep_ms(2)
    stream, ring, fb = spk.stats()
    return {"bytes": got, "peak": peak, "stream": stream, "ring": ring, "feedback": fb,
            "streaming": spk.streaming, "high_speed": spk.high_speed}
