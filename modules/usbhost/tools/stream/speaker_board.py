# Board half of the sound-card stream check: bring up usb-device-audio's
# speaker beside the console, then drain its ring for a while and report what
# arrived. Driven over a held REPL (tools/stream/speaker_check.py).
import time

import usb.device
from usb.device.audio import SpeakerInterface

spk = None


def start(ring_bytes=16384):
    global spk
    spk = SpeakerInterface(ring_bytes=ring_bytes)
    usb.device.get().init(spk, builtin_driver=True)
    return spk.packet


def run(seconds):
    """Drain the ring for this long; returns what came, and its loudest sample."""
    buf = bytearray(1024)
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


def play(seconds, volume=None):
    """Play what the computer sends out of the board's own speaker for this
    long: the ring into the board's PCM output, whose blocking write paces
    the drain at the I2S clock, so the feedback has a real clock to follow.
    Returns the stream's counts and the feedback's range while it played."""
    import board_peripherals
    from audiodev import AudioFormat

    out = board_peripherals.pcm_out(AudioFormat(spk.rate, spk.channels, spk.sample_bytes * 8))
    if volume is not None:
        board_peripherals.audio_power(True, volume=volume)
    # 4 KB a pass (about 21 ms): a smaller read spends more time in the loop
    # than the audio it moves, and the ring overflows behind it.
    buf = bytearray(4096)
    played = 0
    fb_lo = fb_hi = None
    end = time.ticks_add(time.ticks_ms(), int(seconds * 1000))
    # Keep about 30 ms queued at the I2S output and leave the rest in the
    # ring: then the ring's level is the backlog between the computer's clock
    # and this board's, which is what the feedback follows.
    q_target = spk.rate * spk.frame_bytes * 30 // 1000
    mv = memoryview(buf)
    try:
        while time.ticks_diff(end, time.ticks_ms()) > 0:
            spk.service()
            if spk.streaming:
                fb = spk._fb
                fb_lo = fb if fb_lo is None or fb < fb_lo else fb_lo
                fb_hi = fb if fb_hi is None or fb > fb_hi else fb_hi
            room = q_target - out.queued_size()
            if room >= 1024:
                n = spk.ring.readinto(buf, min(room, len(buf)) // spk.frame_bytes * spk.frame_bytes)
                if n:
                    out.write(mv[:n])
                    played += n
                    continue
            time.sleep_ms(2)
    finally:
        out.close()
    stream, ring, fb = spk.stats()
    return {"played": played, "stream": stream, "ring": ring, "feedback": (fb_lo, fb_hi, spk._nominal)}
