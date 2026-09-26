# A/V clock: a looped silent sample plays through the board's sample player,
# which the audio pump pulls at the codec's rate, and the pump's frame counter
# (audiodev.pump.now()) is the master clock; the MJPEG clip (from RAM) is
# slaved to it, a frame decoded when the audio position says it is due and
# dropped when a whole frame late. Reports lateness, drops, and how the audio
# clock ran against the CPU clock over a minute. Silent by construction.
print("av test starting")
import sys
sys.path.insert(0, "/cast")
import time, struct, gc
import jpegdec
import audiodev.pump as pump
import audiocore, array
from board_peripherals import audio_out, AudioFormat
from board_config import fb

PATH = "/cast/test720.mjpeg"
FPS = 30
RATE = 48000
SECONDS = 60
FRAMES_PER_VIDEO = RATE // FPS      # 1600 audio frames per picture
LOG = open("/cast/av_mjpeg.log", "w")


def log(*a):
    line = " ".join(str(x) for x in a)
    print(line)
    LOG.write(line + "\n")
    LOG.flush()


idx = open(PATH + ".idx", "rb").read()
sizes = [struct.unpack_from("<I", idx, 4 * i)[0] for i in range(len(idx) // 4)]
print("loading the clip into RAM")
f = open(PATH, "rb")
clip = bytearray(sum(sizes))
f.readinto(clip)
clip = memoryview(clip)
offsets = []
o = 0
for sz in sizes:
    offsets.append(o)
    o += sz
dec = jpegdec.Decoder()
gc.collect()
log("clip %d frames in RAM; free %d" % (len(sizes), gc.mem_free()))

player = audio_out(AudioFormat(RATE, 2, 16))
silence = audiocore.RawSample(array.array('h', [0] * (RATE // 10 * 2)), sample_rate=RATE, channel_count=2)   # 100 ms of 16-bit zeros
player.play(silence, loop=True)
time.sleep_ms(300)
log("player %r playing %s; pump now %d" % (player, player.playing, pump.now()))
a0 = pump.now()
t0 = time.ticks_ms()
# The pump's counter moves once per block it pulls (100 ms here); between
# steps the CPU clock fills in, capped so a stalled pump never runs ahead.
last_now = a0
last_step = time.ticks_us()
steps = 0
shown = -1
frames = dropped = 0
late_sum = late_max = 0
dec_us = 0
fed = 0
while True:
    n = pump.now()
    if n != last_now:
        last_now = n
        last_step = time.ticks_us()
        steps += 1
    a = last_now + min(time.ticks_diff(time.ticks_us(), last_step) * RATE // 1000000, RATE // 5) - a0
    if a >= SECONDS * RATE or time.ticks_diff(time.ticks_ms(), t0) > (SECONDS + 10) * 1000:
        break
    due = a // FRAMES_PER_VIDEO
    if due <= shown:
        continue
    if due - shown > 1:
        dropped += due - shown - 1
    shown = due
    late = a - due * FRAMES_PER_VIDEO   # audio frames past the due instant
    late_sum += late
    if late > late_max:
        late_max = late
    k = due % len(sizes)
    b = time.ticks_us()
    dec.decode(clip[offsets[k]:offsets[k] + sizes[k]], fb)
    dec_us += time.ticks_diff(time.ticks_us(), b)
    frames += 1
    if frames % 300 == 0:
        log("shown %d dropped %d late avg %.1f ms max %.1f ms decode %d us; audio %.2f s wall %.2f s" % (
            frames, dropped, late_sum / frames / 48, late_max / 48, dec_us // frames, a / RATE, time.ticks_diff(time.ticks_ms(), t0) / 1000))
wall = time.ticks_diff(time.ticks_ms(), t0) / 1000
log("done: %d frames shown, %d dropped, late avg %.1f ms max %.1f ms, decode avg %d us; audio clock %.3f s over wall %.3f s = %+.3f %%; pump stepped %d times (%.0f ms apart)" % (
    frames, dropped, late_sum / max(1, frames) / 48, late_max / 48, dec_us // max(1, frames), (last_now - a0) / RATE, wall, ((last_now - a0) / RATE / wall - 1) * 100, steps, wall * 1000 / max(1, steps)))
player.stop()
dec.close()
LOG.close()
print("AV_DONE")
