# The same Miracast-over-Infrastructure session, aimed at a Roku TV that
# advertises _display._tcp. Video only: nothing in this stream can make a sound.
import sys
sys.path.insert(0, "/cast")
import time, gc
from micecast import Session
from castlive import LiveStreamer
from board_config import fb, display_drv

SINK = "192.168.1.129"        # the 65" TCL Roku TV
SECONDS = 40
SESSION_REQUEST = 0           # None: plain Source Ready; 0: Session Request first, no DTLS, no PIN
LOG = open("/cast/cast_roku.log", "w")
T0 = time.ticks_ms()


def log(*a):
    line = "[%6d] " % time.ticks_diff(time.ticks_ms(), T0) + " ".join(str(x) for x in a)
    print(line)
    LOG.write(line + "\n")
    LOG.flush()


display_drv.fill(0x001F)
display_drv.fill_rect(160, 160, 400, 400, 0xFFE0)
display_drv.fill_rect(280, 280, 160, 160, 0xF800)


class Scene:
    def __init__(self):
        self.n = 0

    def step(self):
        self.n += 1
        x = 60 + (self.n * 6) % 600
        display_drv.fill_rect(0, 60, 720, 40, 0x001F)
        display_drv.fill_rect(x, 60, 60, 40, 0x07E0)


def make(dst_ip, dst_port, server_port):
    return LiveStreamer(dst_ip, dst_port, server_port, fb, fps=30, bitrate=3_000_000, scene=Scene(), seconds=SECONDS, log=log, audio=None)


gc.collect()
s = Session(SINK, name="PyDevices P4", log=log)
s.session_request = SESSION_REQUEST
try:
    result = s.run(make, seconds=SECONDS, idle_after_done=2)
    log("result:", result)
except Exception as e:
    log("EXC", repr(e))
LOG.close()
print("ROKU_DONE")
