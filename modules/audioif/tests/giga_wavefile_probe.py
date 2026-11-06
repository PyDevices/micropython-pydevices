# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Check that `audioio.AudioOut` plays a `WaveFile` on an Arduino GIGA R1, heard
by the board's own ADC.

    mpftp put -d COM5 tests/giga_wavefile_probe.py /flash/giga_wavefile_probe.py
    mpftp exec -d COM5 "import giga_wavefile_probe as p; p.run()"

The probe writes a one-second 551.25 Hz WAV to /flash if there isn't one, plays
it through the DAC, and samples PA4 (the jack's left) at 48 kHz with the ADC.
A tone there swings about 1,500 counts; the DAC at rest swings under 10.

The legs that matter read `playing` while the file plays, as
`while out.playing: pass` does. Each read used to run sixteen blocks of the
pump whatever room the DAC's ring had, which emptied the file's ring and held
the interpreter for 340 ms, and the DAC played silence. The first leg plays a
`RawSample` the same way, so a silent DAC can't pass for a working one.

Every leg prints PASS or FAIL and the last line is the tally.
"""

import array
import math
import os
import struct
import time

import audiocore
import audioio
import pyb

RATE = 22050
WAV = "/flash/t551.wav"
LOUD = 1000             # ADC counts, peak to peak, that mean a tone
FAILS = []


def check(name, ok, detail=""):
    print("%-48s %s %s" % (name, "PASS" if ok else "FAIL", detail))
    if not ok:
        FAILS.append(name)


def write_wav():
    try:
        os.stat(WAV)
        return
    except OSError:
        pass
    block = array.array("h", [int(12000 * math.sin(2 * math.pi * i / 40)) for i in range(1000)])
    with open(WAV, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + RATE * 2) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", RATE * 2))
        for _ in range(RATE // 1000):
            f.write(block)
        f.write(bytes((RATE % 1000) * 2))


def swing(adc):
    buf = array.array("H", bytes(4096))
    tim = pyb.Timer(8, freq=48000)
    adc.read_timed(buf, tim)
    tim.deinit()
    return max(buf) - min(buf)


def read_playing(out, adc, name):
    worst = 0
    for _ in range(4):
        time.sleep_ms(100)
        t0 = time.ticks_us()
        out.playing
        worst = max(worst, time.ticks_diff(time.ticks_us(), t0))
    check(name + ": a playing read is quick", worst < 20000, "%d us" % worst)
    pp = swing(adc)
    check(name + ": the DAC plays a tone", pp > LOUD, "%d counts" % pp)


def run():
    write_wav()
    adc = pyb.ADC(pyb.Pin("PA4"))
    out = audioio.AudioOut(pyb.Pin("PA4"), right_channel=pyb.Pin("PA5"))
    try:
        tone = array.array("h", [int(12000 * math.sin(2 * math.pi * i / 20)) for i in range(400)])
        out.play(audiocore.RawSample(tone, sample_rate=RATE), loop=True)
        read_playing(out, adc, "RawSample")
        out.stop()
        time.sleep_ms(200)
        pp = swing(adc)
        check("stopped: the DAC rests", pp < LOUD, "%d counts" % pp)

        out.play(audiocore.WaveFile(open(WAV, "rb")), loop=True)
        read_playing(out, adc, "WaveFile")

        # The idiom, with a file that ends: it plays, and the wait ends.
        out.play(audiocore.WaveFile(open(WAV, "rb")))
        t0 = time.ticks_ms()
        pp = None
        while out.playing and time.ticks_diff(time.ticks_ms(), t0) < 3000:
            if pp is None and time.ticks_diff(time.ticks_ms(), t0) > 400:
                pp = swing(adc)
        ms = time.ticks_diff(time.ticks_ms(), t0)
        check("while playing: the DAC plays a tone", pp is not None and pp > LOUD, "%s counts" % pp)
        check("while playing: ends after the file", 900 < ms < 1500, "%d ms" % ms)
    finally:
        out.deinit()
    print("%d failed" % len(FAILS) if FAILS else "all passed")
