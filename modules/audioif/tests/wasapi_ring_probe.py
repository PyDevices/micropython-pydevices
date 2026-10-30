"""A WASAPI microphone feeding an audiopump.Ring on its own thread holds pace.

    micropython.exe tests/wasapi_ring_probe.py [secs=30] [in=ID] [out=ID]
        [target=1024] [--fault starve]

Windows only, with real devices: the default ones unless IDs are given. The
microphone writes into a Ring at its own pace (`Input(..., ring=)`), an
`audiopump.DriftResampler` plays it to the speaker at the speaker's pace, and
after the loop has had ten seconds to lock this checks three things:

- the reader thread delivered the microphone's rate (within 0.5% of the
  speaker's, which a lost packet a second would break);
- the Ring never ran dry after the lock (no new underrun);
- the Ring never overflowed.

``--fault starve`` stops the reader thread halfway through (deinit of the
Input while the resampler still plays), so the Ring runs dry and the probe
must exit non-zero.
"""

import sys
import time

import _audioif
import audiobusio
import audiopump

RATE = 48000
opt = {"secs": "30", "in": "", "out": "", "target": "1024"}
fault = None
args = sys.argv[1:]
while args:
    a = args.pop(0)
    if a == "--fault":
        fault = args.pop(0)
        continue
    k, _, v = a.partition("=")
    opt[k] = v
SECS = float(opt["secs"])

print("speaker:", _audioif.endpoint(opt["out"] or None, False)[1])
print("mic:    ", _audioif.endpoint(opt["in"] or None, True)[1])
print("same clock:", _audioif.same_clock(opt["out"] or None, opt["in"] or None))

ring = audiopump.Ring(sample_rate=RATE, channel_count=2, frames=128,
                      capacity=64)
mic = _audioif.Input(sample_rate=RATE, channel_count=2, frames=128,
                     device=opt["in"] or None, ring=ring)
drift = audiopump.DriftResampler(ring, target=int(opt["target"]))
sink = "wasapi" + ("@" + opt["out"] if opt["out"] else "")
out = audiobusio.I2SOut(0, 1, 2, sample_rate=RATE, sink=sink)
out.play(drift, loop=False)

t0 = time.ticks_ms()
mark = None
stopped = False
while True:
    el = time.ticks_diff(time.ticks_ms(), t0)
    if el >= SECS * 1000:
        break
    if mark is None and el >= 10000:
        mark = (el, mic.stats()[0], _audioif.sink_stats()[0], drift.underruns)
    if fault == "starve" and not stopped and el >= SECS * 500:
        mic.deinit()
        stopped = True
    time.sleep_ms(20)
end = (time.ticks_diff(time.ticks_ms(), t0), mic.stats()[0] if not stopped
       else mark[1], _audioif.sink_stats()[0], drift.underruns)
rstats = ring.stats()
out.stop()
out.deinit()
if not stopped:
    mic.deinit()

secs = (end[0] - mark[0]) / 1000
mic_rate = (end[1] - mark[1]) / secs
spk_rate = (end[2] - mark[2]) / secs
under = end[3] - mark[3]
overruns = rstats[4]
print("over %.0f s after the lock: mic %.0f, speaker %.0f frames/s; underruns"
      " %d; ring overruns %d; ppm %+.1f" % (secs, mic_rate, spk_rate, under,
                                           overruns, drift.ppm))
bad = []
if not stopped and abs(mic_rate - spk_rate) > spk_rate * 0.005:
    bad.append("the microphone's rate is off the speaker's by more than 0.5%")
if under:
    bad.append("the Ring ran dry %d times after the lock" % under)
if overruns:
    bad.append("the Ring overflowed %d times" % overruns)
if bad:
    print("FAIL:", "; ".join(bad))
    sys.exit(1)
print("PASS")
