# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""h264enc on the ESP32-P4 -- the media Phase 3 encode gate, board half.

Encodes N frames from h264_frames.py (no camera, no cast) at 720x720, 30 fps,
3 Mbit/s, GOP 30, writes the Annex B stream to OUT/h264enc.h264, and checks
what the board can: every encode returns bytes, the first frame and every
30th is an IDR, force_idr() makes the next one an IDR, and a second Encoder
while this one is open raises OSError "busy" without disturbing it.
check_h264_ffmpeg.py then has ffmpeg decode the file on the PC.

Then the same frames through a pipelined Encoder: the first encode() returns
b"", every later one the frame before, flush() the last; the access units
match the plain Encoder's byte for byte up to the forced IDR; and the time a
frame keeps its caller (wall_us) is printed beside the plain one's.

    run on the board with h264_frames.py beside it (mpftp probe; see the README),
    MicroPython or CircuitPython, with a /jt folder for the stream
"""

import sys
import time

try:
    from hashlib import sha256
except ImportError:
    sha256 = None

import h264enc

try:
    from time import ticks_diff, ticks_ms
except ImportError:  # CircuitPython: monotonic_ns doesn't wrap
    def ticks_ms():
        return time.monotonic_ns() // 1_000_000

    def ticks_diff(a, b):
        return a - b

import h264_frames as frames

N = 60
OUT = "/jt"


def main():
    t0 = ticks_ms()
    bg = frames.base()
    buf = bytearray(len(bg))
    print("background built in %d ms" % ticks_diff(ticks_ms(), t0))
    enc = h264enc.Encoder(frames.W, frames.H, 30, 30, 3_000_000)
    print("Encoder", enc.width, "x", enc.height)

    # a second user while this one is open
    try:
        h264enc.Encoder(frames.W, frames.H)
        raise AssertionError("a second Encoder opened while the first was open")
    except OSError as e:
        assert "busy" in str(e), e
        print("second Encoder: OSError:", e)

    keys = []
    total = 0
    enc_us = 0
    wall_us = 0
    digests = []
    with open(OUT + "/h264enc.h264", "wb") as f:
        for k in range(N):
            frames.frame(bg, k, buf)
            if k == 45:
                enc.force_idr()
            au = enc.encode(buf)
            assert isinstance(au, bytes) and len(au) > 0
            assert au[:4] == b"\x00\x00\x00\x01" or au[:3] == b"\x00\x00\x01", au[:8]
            if enc.keyframe:
                keys.append(k)
            total += len(au)
            enc_us += enc.stats()["enc_us"] + enc.stats()["ppa_us"]
            wall_us += enc.stats()["wall_us"]
            digests.append(sha256(au).digest() if sha256 else len(au))
            f.write(au)
    st = enc.stats()
    print("frames", st["frames"], "keyframes at", keys, "bytes", total,
          "mean encode+ppa %d us" % (enc_us // N))
    assert keys[0] == 0 and 30 in keys and 45 in keys, keys
    assert st["frames"] == N and st["errors"] == 0

    # the first session is untouched by the refused second one, and close()
    # gives the encoder back
    enc.close()
    again = h264enc.Encoder(160, 96)
    again.encode(bytes(160 * 96 * 2))
    again.close()
    try:
        enc.encode(buf)
        raise AssertionError("encode() after close()")
    except ValueError:
        pass
    print("after close(): a new Encoder opens; the closed one refuses encode()")
    pipelined(bg, buf, digests, wall_us // N)
    print("h264enc board test passed, %d frames, %.1f s" % (N, ticks_diff(ticks_ms(), t0) / 1000))


def pipelined(bg, buf, digests, plain_wall_us):
    # The same frames, pipelined, without the forced IDR: up to frame 44 the
    # two streams must be the same bytes.
    enc = h264enc.Encoder(frames.W, frames.H, 30, 30, 3_000_000, pipeline=True)
    assert enc.pipeline
    aus = []
    wall_us = 0
    for k in range(N):
        frames.frame(bg, k, buf)
        au = enc.encode(buf)
        wall_us += enc.stats()["wall_us"]
        if k == 0:
            assert au == b"", "the first pipelined encode() returned %d bytes" % len(au)
        else:
            assert len(au) > 0 and au[:3] in (b"\x00\x00\x00", b"\x00\x00\x01"), au[:8]
            aus.append(au)
    aus.append(enc.flush())
    assert enc.flush() == b"", "a second flush() returned bytes"
    st = enc.stats()
    assert len(aus) == N and st["frames"] == N and st["errors"] == 0, (len(aus), st)
    for k in range(45):
        got = sha256(aus[k]).digest() if sha256 else len(aus[k])
        assert got == digests[k], "pipelined frame %d differs from the plain encode" % k
    assert enc.close() == b""
    print("pipelined: %d frames, the same bytes as the plain Encoder up to frame 44" % N)
    print("mean wall per frame: plain %d us, pipelined %d us" % (plain_wall_us, wall_us // N))


main()
