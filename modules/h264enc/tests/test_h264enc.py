"""h264enc on the ESP32-P4 -- the media Phase 3 encode gate, board half.

Encodes N frames from h264_frames.py (no camera, no cast) at 720x720, 30 fps,
3 Mbit/s, GOP 30, writes the Annex B stream to OUT/h264enc.h264, and checks
what the board can: every encode returns bytes, the first frame and every
30th is an IDR, force_idr() makes the next one an IDR, and a second Encoder
while this one is open raises OSError "busy" without disturbing it.
check_h264_ffmpeg.py then has ffmpeg decode the file on the PC.

    run on the board with h264_frames.py beside it (mpftp probe; see the README)
"""

import sys
import time

import h264enc

import h264_frames as frames

N = 60
OUT = "/jt"


def main():
    t0 = time.ticks_ms()
    bg = frames.base()
    buf = bytearray(len(bg))
    print("background built in %d ms" % time.ticks_diff(time.ticks_ms(), t0))
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
    print("h264enc board test passed, %d frames, %.1f s" % (N, time.ticks_diff(time.ticks_ms(), t0) / 1000))


main()
