"""ffprobe decodes what test_castif_hls served: each kept segment alone and
the playlist, and /stats parses as JSON. A planted fault (a segment cut in
half) must fail.

    python check_castif_hls.py OUT_DIR
"""

import json
import os
import subprocess
import sys


def frames(path, fmt=None):
    cmd = ["ffprobe", "-v", "error"] + (["-f", fmt] if fmt else []) + [
        "-count_frames", "-select_streams", "v:0", "-show_entries", "stream=nb_read_frames",
        "-of", "default=nw=1:nk=1", path]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if r.returncode != 0 or r.stderr.strip():
        return None, r.stderr.strip()[-300:]
    nums = [ln for ln in r.stdout.split() if ln.isdigit()]
    return (int(nums[0]) if nums else None), r.stdout.strip()


def main():
    out = sys.argv[1]
    # the served playlist is live (no end tag), so ffmpeg would wait for the
    # next one: probe a copy that ends where this one does
    with open(os.path.join(out, "stream.m3u8")) as f:
        live = f.read()
    with open(os.path.join(out, "ended.m3u8"), "w") as f:
        f.write(live + "#EXT-X-ENDLIST\n")
    checks = [("seg%d.ts" % n, 30, None) for n in range(4, 11)] + [("ended.m3u8", 90, "hls")]
    bad = 0
    for name, want, fmt in checks:
        n, info = frames(os.path.join(out, name), fmt)
        ok = n == want
        bad += not ok
        print("%-12s %s frames (want %d) %s" % (name, n, want, info if not ok else "ok"))

    with open(os.path.join(out, "stats.json")) as f:
        stats = json.load(f)
    keys = {"now", "frames", "offers", "requests", "overruns", "player", "began", "first_fetch", "first_began"}
    missing = keys - set(stats)
    bad += bool(missing)
    print("stats.json   %s" % ("missing %s" % sorted(missing) if missing else "parses, every key present"))

    with open(os.path.join(out, "seg4.ts"), "rb") as f:
        ts = f.read()
    planted = os.path.join(out, "planted.ts")
    with open(planted, "wb") as f:
        f.write(ts[:len(ts) // 2 // 188 * 188])
    n, info = frames(planted, "mpegts")
    verdict = "fails, as it must" if n != 30 else "PASSED, so the check is blind"
    print("planted fault (half a segment): %s frames: %s" % (n, verdict))
    bad += n == 30
    print("FAIL" if bad else "PASS")
    sys.exit(1 if bad else 0)


main()
