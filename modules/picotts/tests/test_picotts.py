"""picotts on the unix port: the engine renders speech, and the API around it.

Run with the unix MicroPython built with --modules picotts, and the folder
holding en_US_ta.bin and en_US_lh0_sg.bin:

    builds/unix/standard/micropython -X heapsize=8M \\
        modules/picotts/tests/test_picotts.py <voice folder> [out.wav]

With out.wav it also writes the first sentence as a 16 kHz mono WAV.
Plain asserts; prints OK at the end.
"""

import struct
import sys
import time

import _picotts
import picotts

VOICE = sys.argv[1] if len(sys.argv) > 1 else "voices"
WAV = sys.argv[2] if len(sys.argv) > 2 else None

TEXT = "Hello from Micro Python. The time is ten thirty."


def load(name):
    with open(VOICE + "/" + name, "rb") as f:
        return f.read()


def render(engine, text, piece=160):
    """Every sample the engine makes for text, in pieces of `piece`."""
    engine.start(text.encode() + b"\0")
    out = bytearray()
    buf = _picotts.samples(piece)
    while engine.speaking:
        n = engine.render(buf)
        out += bytes(buf)[: 2 * n]
    return out


def peak(pcm):
    top = 0
    for i in range(0, len(pcm), 2):
        v = struct.unpack_from("<h", pcm, i)[0]
        top = max(top, -v if v < 0 else v)
    return top


def write_wav(path, pcm):
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16))
        f.write(b"data" + struct.pack("<I", len(pcm)))
        f.write(pcm)


# The module's constants and helpers.
assert _picotts.SAMPLE_RATE == 16000
assert _picotts.psram() == -1  # no PSRAM idea on the desktop
s = _picotts.samples(4)
assert len(s) == 4 and s[0] == 0 and s[3] == 0
s[1] = -1234
assert s[1] == -1234

ta, sg = load("en_US_ta.bin"), load("en_US_lh0_sg.bin")

# A work area that is too small is refused before the engine sees it.
try:
    _picotts.Engine(ta, sg, bytearray(1000))
    raise AssertionError("small memory accepted")
except ValueError:
    pass

# Garbage voice data fails cleanly.
try:
    _picotts.Engine(b"x" * 100, b"y" * 100, bytearray(1100000))
    raise AssertionError("bad voice accepted")
except RuntimeError as e:
    print("bad voice ->", e)

engine = _picotts.Engine(ta, sg, bytearray(1100000))

# Text without the closing NUL is refused.
try:
    engine.start(b"no nul")
    raise AssertionError("text without NUL accepted")
except ValueError:
    pass

t0 = time.ticks_ms()
pcm = render(engine, TEXT)
ms = time.ticks_diff(time.ticks_ms(), t0)
seconds = len(pcm) / 32000
print("rendered %.2f s of speech in %d ms, peak %d" % (seconds, ms, peak(pcm)))
assert 2.0 < seconds < 6.0, seconds
assert peak(pcm) > 3000

# The same text renders the same samples, and the piece size doesn't matter.
assert render(engine, TEXT, piece=1000) == pcm
assert render(engine, TEXT, piece=7) == pcm

# stop() drops the rest of a text.
engine.start(TEXT.encode() + b"\0")
engine.render(_picotts.samples(160))
engine.stop()
assert not engine.speaking

if WAV:
    write_wav(WAV, pcm)
    print("wrote", WAV)

engine.deinit()
engine.deinit()  # twice is fine
try:
    engine.start(b"x\0")
    raise AssertionError("deinitialized engine used")
except ValueError:
    pass

# The TTS class says where the voice files should go when they aren't there.
try:
    picotts._voice_dir("/nonexistent", "en-US")
    raise AssertionError("missing voice accepted")
except OSError as e:
    print("missing voice ->", e)
assert picotts._voice_dir(VOICE, "en-US") == VOICE

print("OK")
