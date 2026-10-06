"""ffmpeg on the PC decodes what test_h264enc.py encoded on the board.

Thresholds fixed before the first run (2026-10-06): ffmpeg decodes exactly N
frames with no errors; against h264_frames.py's sources (RGB565 widened as a display
does) the mean PSNR is at least 30 dB and no frame is under 25 dB. A planted
fault, the decoded frames compared one frame late, must fail that.

    python check_h264_ffmpeg.py h264enc.h264 [N]
"""

import math
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import h264_frames as frames  # noqa: E402


def source(bg, k):
    buf = bytearray(len(bg))
    frames.frame(bg, k, buf)
    v = np.frombuffer(bytes(buf), dtype="<u2").reshape(frames.H, frames.W).astype(np.int32)
    r, g, b = v >> 11, (v >> 5) & 63, v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float64)


def psnr(a, b):
    mse = ((a - b) ** 2).mean()
    return 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)


def main():
    path = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    run = subprocess.run(["ffmpeg", "-v", "error", "-f", "h264", "-i", path, "-f", "rawvideo",
                          "-pix_fmt", "rgb24", "-"], capture_output=True)
    assert run.returncode == 0 and not run.stderr.strip(), run.stderr.decode()[-2000:]
    size = frames.W * frames.H * 3
    count = len(run.stdout) // size
    print("ffmpeg decoded %d frames (expected %d), no errors" % (count, n))
    assert count == n and len(run.stdout) == n * size
    dec = np.frombuffer(run.stdout, dtype=np.uint8).reshape(n, frames.H, frames.W, 3).astype(np.float64)
    bg = frames.base()
    src = [source(bg, k) for k in range(n)]
    p = [psnr(src[k], dec[k]) for k in range(n)]
    print("PSNR mean %.2f dB (>= 30), min %.2f dB at frame %d (>= 25)" % (sum(p) / n, min(p), p.index(min(p))))
    late = [psnr(src[k], dec[k + 1]) for k in range(n - 1)]
    print("planted fault, one frame late: mean %.2f dB, min %.2f dB (must fail)" % (sum(late) / len(late), min(late)))
    assert sum(p) / n >= 30 and min(p) >= 25
    assert not (sum(late) / len(late) >= 30 and min(late) >= 25), "the check cannot tell a frame off by one"
    print("PASS")


main()
