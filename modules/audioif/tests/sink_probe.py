"""The driver's file sink hands over exactly what the engine pulled.

    <interpreter> tests/sink_probe.py [scratch-dir] [--fault WHICH]

The engine (audiodsp) computes an FNV-1a 64 digest of every byte it pulls
and leaves it in its status block. The sink is **this** repository: the one
place a block stops being the engine's and becomes a platform's -- a file
here, an I2S channel on a board. So the check is the engine's own digest
against a digest of the file, and the only thing between them is the code
in this repo.

It matters because a sink is where a byte count quietly stops matching. A
short write, a buffer handed over twice, a block dropped when the writer is
behind: none of those raise, and all of them are audible.

Faults: ``flip`` changes one byte of the file before it is hashed;
``truncate`` cuts the last block off. Each must make this exit non-zero.
"""

import os
import struct
import sys
from array import array

import audiocore
import audiomixer
import audiopump

RATE = 48000
CHANNELS = 2
BLOCK_FRAMES = 256
BLOCK_BYTES = BLOCK_FRAMES * CHANNELS * 2
BLOCKS = 40

_ARGS = sys.argv[1:]
FAULT = None
if "--fault" in _ARGS:
    _AT = _ARGS.index("--fault")
    FAULT = _ARGS[_AT + 1]
    del _ARGS[_AT:_AT + 2]
DIR = _ARGS[0] if _ARGS else os.getenv("PUMP_PROBE_TMP", "/tmp")


def digest(data):
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def source():
    frames = BLOCK_FRAMES * 4
    data = array("h", bytes(2 * frames * CHANNELS))
    for index in range(frames):
        value = ((index * 7 + 11) ^ (index >> 8)) & 0x7FF
        for channel in range(CHANNELS):
            data[index * CHANNELS + channel] = value - 1024
    return audiocore.RawSample(data, sample_rate=RATE,
                               channel_count=CHANNELS)


def main():
    path = DIR + "/audiopump_sink.raw"
    mixer = audiomixer.Mixer(voice_count=1, sample_rate=RATE,
                             channel_count=CHANNELS, bits_per_sample=16,
                             samples_signed=True, buffer_size=2 * BLOCK_BYTES)
    mixer.voice[0].level = 1.0
    mixer.play(source(), voice=0, loop=True)

    block = bytearray(audiopump.STATUS_BYTES)
    audiopump.pull(mixer, BLOCKS, block, path)
    words = struct.unpack_from("<%dQ" % audiopump.STATUS_WORDS, block, 0)

    with open(path, "rb") as handle:
        out = bytearray(handle.read())
    if FAULT == "flip" and len(out) > 999:
        out[999] ^= 0x01
    if FAULT == "truncate" and len(out) > BLOCK_BYTES:
        out = out[:len(out) - BLOCK_BYTES]

    same = digest(out) == words[2] and len(out) == words[1]
    print("pulled %d blocks, %d bytes; sink holds %d bytes"
          % (words[0], words[1], len(out)))
    print("engine digest %016x / sink digest %016x  %s"
          % (words[2], digest(out), "SAME" if same else "DIFFERENT"))
    print("VERDICT:", "the sink is faithful" if same else "BROKEN")
    return 0 if same else 1


sys.exit(main())
