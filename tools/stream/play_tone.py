"""Play a tone into a named output device (Windows Python, sounddevice).

    python play_tone.py NAME SECONDS [HZ] [LEVEL] [--beeps]

Plays a stereo 48 kHz 16-bit sine to the first output device whose name
contains NAME, without touching the default device. Prints a JSON line.
--beeps plays 200 ms beeps every 500 ms at HZ, 1.5 HZ and 2 HZ in turn
instead: a laptop microphone's noise suppression silences a steady tone
within a second, and beeps get through it.
"""
import json
import sys

import numpy as np
import sounddevice as sd

beeps = "--beeps" in sys.argv
args = [a for a in sys.argv[1:] if a != "--beeps"]
name, seconds = args[0], float(args[1])
hz = float(args[2]) if len(args) > 2 else 1000.0
level = float(args[3]) if len(args) > 3 else 0.5
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
if beeps:
    step = (t // 0.5).astype(int)
    f = hz * np.array([1.0, 1.5, 2.0])[step % 3]
    gate = (t % 0.5) < 0.2
    tone = (np.sin(2 * np.pi * f * t) * gate * level * 32767).astype(np.int16)
else:
    tone = (np.sin(2 * np.pi * hz * t) * level * 32767).astype(np.int16)
stereo = np.column_stack((tone, tone))
sd.play(stereo, 48000, device=index, blocking=True)
print(json.dumps({"device": dev["name"], "hostapi": apis[dev["hostapi"]]["name"], "seconds": seconds,
                  "expected_peak": int(level * 32767)}))
