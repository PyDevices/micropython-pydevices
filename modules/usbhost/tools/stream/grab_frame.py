"""Grab a frame from a named webcam and check it shows the colour bars
(Windows Python: ffmpeg's dshow, numpy, Pillow).

    python grab_frame.py NAME OUT.png

Prints a JSON line: the frame's size, each bar's mean RGB, and whether the
eight bars came out in order (white, yellow, cyan, green, magenta, red, blue,
black), each within a generous tolerance.
"""
import json
import subprocess
import sys

import numpy as np
from PIL import Image

name, out = sys.argv[1], sys.argv[2]
FF = "C:/Users/bradb/AppData/Local/Microsoft/WinGet/Packages/Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe/ffmpeg-9.0.2-full_build/bin/ffmpeg.exe"
r = subprocess.run([FF, "-hide_banner", "-loglevel", "error", "-f", "dshow", "-i", "video=" + name,
                    "-frames:v", "5", "-update", "1", "-y", out], capture_output=True, text=True, timeout=60)
if r.returncode:
    print(json.dumps({"pass": False, "error": r.stderr[-300:]}))
    sys.exit(1)
img = np.asarray(Image.open(out).convert("RGB")).astype(int)
h, w, _ = img.shape
want = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
got, ok = [], True
for i, rgb in enumerate(want):
    x0, x1 = i * w // 8 + w // 32, (i + 1) * w // 8 - w // 32
    mean = img[h // 4: 3 * h // 4, x0:x1].reshape(-1, 3).mean(axis=0)
    got.append([int(c) for c in mean])
    ok = ok and all(abs(m - c) < 80 for m, c in zip(mean, rgb))
print(json.dumps({"pass": ok, "size": [w, h], "bars": got}))
sys.exit(0 if ok else 1)
