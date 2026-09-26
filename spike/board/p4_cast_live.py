# Cast the panel live: the hardware encoder reads the MIPI-DSI framebuffer,
# tsmux packetizes, RTP goes to the Windows receiver. Draws a moving block on
# the panel so there is something to see; the spectrum meter if it's aboard.
import sys
sys.path.insert(0, "/cast")
import socket, time, gc
from micecast import Session
from board_config import fb, display_drv

SINK = "192.168.1.143"
FPS = 30
BITRATE = 3_000_000
SECONDS = 40
LOG = open("/cast/cast_live.log", "w")
T0 = time.ticks_ms()


def log(*a):
    line = "[%6d] " % time.ticks_diff(time.ticks_ms(), T0) + " ".join(str(x) for x in a)
    print(line)
    LOG.write(line + "\n")
    LOG.flush()


class Scene:
    """A bouncing block and a frame counter bar, drawn straight into the panel."""

    def __init__(self):
        self.x, self.y, self.dx, self.dy = 40, 60, 9, 7
        self.n = 0
        w, h = display_drv.width, display_drv.height
        display_drv.fill(0x0000)
        for i in range(0, w, 60):
            display_drv.fill_rect(i, 0, 30, h, 0x2104)
        self.w, self.h = w, h

    def step(self):
        display_drv.fill_rect(self.x, self.y, 120, 120, 0x0000)
        self.x += self.dx
        self.y += self.dy
        if self.x < 0 or self.x > self.w - 120:
            self.dx = -self.dx
            self.x = min(max(self.x, 0), self.w - 120)
        if self.y < 0 or self.y > self.h - 140:
            self.dy = -self.dy
            self.y = min(max(self.y, 0), self.h - 140)
        display_drv.fill_rect(self.x, self.y, 120, 120, 0xF800 if (self.n // 30) % 2 else 0x07E0)
        display_drv.fill_rect(0, self.h - 20, max(1, (self.n * 4) % self.w), 20, 0xFFE0)
        self.n += 1
        try:
            display_drv.show()
        except Exception:
            pass


from castlive import LiveStreamer


def make(dst_ip, dst_port, server_port):
    return LiveStreamer(dst_ip, dst_port, server_port, fb, fps=FPS, bitrate=BITRATE, scene=Scene(), seconds=SECONDS, log=log)


gc.collect()
s = Session(SINK, log=log)
try:
    result = s.run(make, seconds=SECONDS + 15, idle_after_done=2)
    log("result:", result)
except Exception as e:
    log("EXC", repr(e))
LOG.close()
print("LIVE_DONE")
