"""The board half's stand-in on a PC: ffmpeg's x264 encodes h264_frames.py's
frames with the settings hardware/test_h264enc.py gives h264enc (720x720,
30 fps, 3 Mbit/s, a keyframe every 30), so CI can run check_h264_ffmpeg.py
on a stream without a board.

    python encode_h264_ffmpeg.py OUT.h264 [N]
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import h264_frames as frames  # noqa: E402


def main():
    out = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    cmd = ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb565le",
           "-s", "%dx%d" % (frames.W, frames.H), "-r", "30", "-i", "-",
           "-c:v", "libx264", "-pix_fmt", "yuv420p", "-b:v", "3M", "-g", "30", "-bf", "0",
           "-f", "h264", out]
    enc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    bg = frames.base()
    buf = bytearray(len(bg))
    for k in range(n):
        frames.frame(bg, k, buf)
        enc.stdin.write(buf)
    enc.stdin.close()
    assert enc.wait(timeout=300) == 0, "ffmpeg failed"
    print("wrote %d frames to %s" % (n, out))


main()
