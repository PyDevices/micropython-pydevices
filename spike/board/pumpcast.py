# pumpcast.py -- play audio through the P4's audio pump AND cast it to the TV,
# so what the pump plays is what the TV gets. The cast pulls 10 ms LPCM blocks
# from a per-block callback; here that callback writes each block into the pump
# (audiodev.pump.attach_stream -> the board's speaker) and returns the same
# block to the cast (castlive -> the TV over Miracast). A looping melody is
# built once into PSRAM so casting stays cheap.
#
# For a real smart-home app: whatever audio you want on both the P4 and the TV,
# write into a PumpCast block source. The demo material is a short arpeggio.
import math
from audiodev import pump, AudioFormat

RATE = 48000
BLOCK_FRAMES = RATE // 100          # 480 frames = 10 ms
BLOCK_BYTES = BLOCK_FRAMES * 4      # 1920 bytes, stereo s16
# C5 E5 G5 C6 up and back to G4 -- an arpeggio that loops cleanly enough.
NOTES = [(523, 250), (659, 250), (784, 250), (1047, 250),
         (784, 250), (659, 250), (523, 250), (392, 250)]


def build_melody(notes=NOTES, amp=4000):
    frames = 0
    for _, ms in notes:
        frames += RATE * ms // 1000
    frames = ((frames + BLOCK_FRAMES - 1) // BLOCK_FRAMES) * BLOCK_FRAMES
    buf = bytearray(frames * 4)
    phase = 0.0
    idx = 0
    for hz, ms in notes:
        n = RATE * ms // 1000
        step = 2 * math.pi * hz / RATE
        for _ in range(n):
            v = int(amp * math.sin(phase))
            phase += step
            hi = (v >> 8) & 0xFF
            lo = v & 0xFF
            buf[idx] = hi; buf[idx + 1] = lo
            buf[idx + 2] = hi; buf[idx + 3] = lo
            idx += 4
    return buf, frames


class PumpCast:
    """A looping melody as a cast audio source that also feeds the pump."""

    def __init__(self, melody=None, echo_to_pump=True, log=print):
        self.buf, self.frames = melody if melody else build_melody()
        self.mv = memoryview(self.buf)
        self.total = self.frames * 4
        self.pos = 0
        self.stream = None
        self.log = log
        if echo_to_pump:
            try:
                # a deeper ring so the cast's ~100 ms-ahead bursts fit
                self.stream = pump.attach_stream(AudioFormat(RATE, 2, 16), capacity=32)
                log("pumpcast: pump output attached (%d bytes free)" % self.stream.space())
            except Exception as e:
                log("pumpcast: no pump output (%r); casting audio only" % (e,))
                self.stream = None

    def block(self):
        end = self.pos + BLOCK_BYTES
        if end <= self.total:
            b = bytes(self.mv[self.pos:end])
            self.pos = 0 if end == self.total else end
        else:
            rest = end - self.total
            b = bytes(self.mv[self.pos:self.total]) + bytes(self.mv[0:rest])
            self.pos = rest
        s = self.stream
        if s is not None:
            try:
                if s.space() >= BLOCK_BYTES:
                    s.write(b)
            except Exception:
                pass
        return b

    def close(self):
        if self.stream is not None:
            try:
                self.stream.deinit()
            except Exception:
                pass
            self.stream = None
