"""The CircuitPython `audiobusio` docstring example, run unmodified.

This is the program in CircuitPython 10.3.0's own
`shared-bindings/audiobusio/I2SOut.c` docstring, character for character
apart from the three pin arguments (a desktop has no `board`). If it plays
here, a CircuitPython audio program runs on this firmware and what it gets
underneath is the pump.

    <interpreter> tests/cp_example_probe.py [sink.raw]
"""

import sys

import audiobusio
import audiocore
import array
import time
import math

SINK = sys.argv[1] if len(sys.argv) > 1 else None

# Generate one period of sine wave.
length = 8000 // 440
sine_wave = array.array("H", [0] * length)
for i in range(length):
    sine_wave[i] = int(math.sin(math.pi * 2 * i / length) * (2 ** 15) + 2 ** 15)

sine_wave = audiocore.RawSample(sine_wave, sample_rate=8000)
i2s = audiobusio.I2SOut(1, 0, 9, sink=SINK)
i2s.play(sine_wave, loop=True)
time.sleep(1)
i2s.stop()

print("played", "loop for 1 s")
