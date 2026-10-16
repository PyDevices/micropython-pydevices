"""Play a tone into a named output device (Windows Python, sounddevice).

    python play_tone.py NAME SECONDS [HZ] [LEVEL]

Plays a stereo 48 kHz 16-bit sine to the first output device whose name
contains NAME, without touching the default device. Prints a JSON line.
"""
import json
import sys

import numpy as np
import sounddevice as sd

name, seconds = sys.argv[1], float(sys.argv[2])
hz = float(sys.argv[3]) if len(sys.argv) > 3 else 1000.0
level = float(sys.argv[4]) if len(sys.argv) > 4 else 0.5
devs = [(i, d) for i, d in enumerate(sd.query_devices())
        if name.lower() in d["name"].lower() and d["max_output_channels"] >= 2]
if not devs:
    print(json.dumps({"error": "no output device matching %r" % name,
                      "outputs": [d["name"] for d in sd.query_devices() if d["max_output_channels"]]}))
    sys.exit(1)
# Prefer WASAPI shared mode's entry when there are several.
apis = sd.query_hostapis()
devs.sort(key=lambda p: 0 if "WASAPI" in apis[p[1]["hostapi"]]["name"] else 1)
index, dev = devs[0]
t = np.arange(int(48000 * seconds)) / 48000.0
tone = (np.sin(2 * np.pi * hz * t) * level * 32767).astype(np.int16)
stereo = np.column_stack((tone, tone))
sd.play(stereo, 48000, device=index, blocking=True)
print(json.dumps({"device": dev["name"], "hostapi": apis[dev["hostapi"]]["name"], "seconds": seconds,
                  "expected_peak": int(level * 32767)}))
