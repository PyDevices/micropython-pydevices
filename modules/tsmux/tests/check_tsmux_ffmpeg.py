"""ffmpeg decodes what test_tsmux.py muxed: the stream, each segment alone,
and the playlist. A planted fault (the stream with every sync byte zeroed)
must fail.

    python check_tsmux_ffmpeg.py OUT_DIR
"""

import os
import subprocess
import sys


def frames(path, fmt=None):
    cmd = ["ffprobe", "-v", "error"] + (["-f", fmt] if fmt else []) + [
        "-count_frames", "-select_streams", "v:0", "-show_entries", "stream=nb_read_frames",
        "-of", "default=nw=1:nk=1", path]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or r.stderr.strip():
        return None, r.stderr.strip()[-300:]
    nums = [ln for ln in r.stdout.split() if ln.isdigit()]
    return (int(nums[0]) if nums else None), r.stdout.strip()


def main():
    out = sys.argv[1]
    checks = [("stream.ts", 90, None), ("seg0.ts", 30, None), ("seg1.ts", 30, None), ("stream.m3u8", 60, "hls")]
    bad = 0
    for name, want, fmt in checks:
        n, info = frames(os.path.join(out, name), fmt)
        ok = n == want
        bad += not ok
        print("%-12s %s frames (want %d) %s" % (name, n, want, info if not ok else "ok"))
    with open(os.path.join(out, "stream.ts"), "rb") as f:
        ts = bytearray(f.read())
    for i in range(0, len(ts), 188):
        ts[i] = 0
    planted = os.path.join(out, "planted.ts")
    with open(planted, "wb") as f:
        f.write(ts)
    n, info = frames(planted, "mpegts")
    print("planted fault (no sync bytes): %s frames: %s" % (n, "fails, as it must" if n != 90 else "PASSED, so the check is blind"))
    bad += n == 90
    print("FAIL" if bad else "PASS")
    sys.exit(1 if bad else 0)


main()
