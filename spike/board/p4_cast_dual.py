# Dual display: the panel shows the spectrum meter's bars; the laptop shows a
# full-width scrolling spectrogram of the same music, drawn into a second
# 1280x720 framebuffer that only the encoder ever sees.
import sys
sys.path.insert(0, "/cast")
sys.path.insert(0, "/spectrum")
import time, gc, framebuf, uctypes
from micecast import Session
from castlive import LiveStreamer
from board_config import fb, display_drv
from displaydev import env_set

SINK = "192.168.1.143"
SECONDS = 90
W, H = 1280, 720
BANDS = 64
ROWS = 4          # rows the waterfall advances per cast frame
TOP = 28          # the title strip; the waterfall scrolls under it
LOG = open("/cast/cast_dual.log", "w")
T0 = time.ticks_ms()


def log(*a):
    line = "[%6d] " % time.ticks_diff(time.ticks_ms(), T0) + " ".join(str(x) for x in a)
    print(line)
    LOG.write(line + "\n")
    LOG.flush()


# The panel: the meter, throttled so its timer ticks leave the cast loop room.
env_set("SPECTRUM_SOURCE", "fake")
import spectrum
spectrum.FRAME_MS = 200   # 5 fps on the panel for this demo: its ticks cost the cast loop
spectrum.start()
log("meter on the panel")
from fake_music import FakeMusic
music = FakeMusic(BANDS)      # the same seeded loop, at spectrogram resolution
t_music = time.ticks_ms()

# The second framebuffer, cache-line aligned for the PPA, in PSRAM.
raw = bytearray(W * H * 2 + 128)
off = (-uctypes.addressof(raw)) % 128
buf2 = memoryview(raw)[off:off + W * H * 2]
fb2 = framebuf.FrameBuffer(buf2, W, H, framebuf.RGB565)
fb2.fill(0)


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


# level 0..1 -> deep blue, blue, cyan, white, then a hot yellow at the top
_PAL = []
for i in range(64):
    t = i / 63
    if t < 0.35:
        u = t / 0.35
        c = rgb565(0, int(20 + 60 * u), int(60 + 160 * u))
    elif t < 0.7:
        u = (t - 0.35) / 0.35
        c = rgb565(int(40 * u), int(80 + 175 * u), int(220 + 35 * u))
    else:
        u = (t - 0.7) / 0.3
        c = rgb565(int(40 + 215 * u), 255, int(255 - 200 * u))
    _PAL.append(c)


class Waterfall:
    def __init__(self):
        self.n = 0
        self.colw = W // BANDS
        # a title strip stays at the top; the waterfall scrolls under it
        fb2.fill_rect(0, 0, W, 28, rgb565(16, 16, 24))
        fb2.text("PyDevices P4  -  the same music, two views: bars on the panel, this on the laptop", 12, 10, rgb565(200, 210, 230))

    def step(self):
        levels = music.levels(time.ticks_diff(time.ticks_ms(), t_music) / 1000)
        # everything under the title moves up: one memmove of the buffer
        # (framebuf.scroll walks every pixel and took 210 ms a frame)
        buf2[TOP * W * 2:(H - ROWS) * W * 2] = buf2[(TOP + ROWS) * W * 2:H * W * 2]
        y = H - ROWS
        x = 0
        for lv in levels:
            k = int(lv * 63)
            fb2.fill_rect(x, y, self.colw - 1, ROWS, _PAL[k if k < 64 else 63])
            x += self.colw
        self.n += 1


gc.collect()
log("second framebuffer at", hex(uctypes.addressof(buf2)), "free", gc.mem_free())


def make(dst_ip, dst_port, server_port):
    return LiveStreamer(dst_ip, dst_port, server_port, buf2, size=(W, H), canvas=(W, H), fps=30,
                        bitrate=4_000_000, scene=Waterfall(), seconds=SECONDS, log=log)


s = Session(SINK, log=log)
try:
    result = s.run(make, seconds=SECONDS + 15, idle_after_done=2)
    log("result:", result)
except Exception as e:
    log("EXC", repr(e))
LOG.close()
print("DUAL_DONE")
