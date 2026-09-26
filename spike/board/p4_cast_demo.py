# The demo: the spectrum meter (fake music) on the panel, cast live to the laptop.
import sys
sys.path.insert(0, "/cast")
sys.path.insert(0, "/spectrum")
import time, gc
from micecast import Session
from castlive import LiveStreamer
from board_config import fb, display_drv
from displaydev import env_set

SINK = "192.168.1.143"
SECONDS = 60
LOG = open("/cast/cast_demo.log", "w")
T0 = time.ticks_ms()


def log(*a):
    line = "[%6d] " % time.ticks_diff(time.ticks_ms(), T0) + " ".join(str(x) for x in a)
    print(line)
    LOG.write(line + "\n")
    LOG.flush()


env_set("SPECTRUM_SOURCE", "fake")
import spectrum
# The meter's timer ticks land between the cast loop's bytecodes; at its full
# rate they starve the Python packetizer (3 ms became 10 s a frame). Until the
# cast runs as a C task, the meter gets 10 fps here.
spectrum.FRAME_MS = 100
spectrum.start()          # half the bands, bottom half of the panel, fake music
log("spectrum meter started")
gc.collect()


def make(dst_ip, dst_port, server_port):
    return LiveStreamer(dst_ip, dst_port, server_port, fb, fps=30, bitrate=4_000_000, seconds=SECONDS, log=log)


s = Session(SINK, log=log)
try:
    result = s.run(make, seconds=SECONDS + 15, idle_after_done=2)
    log("result:", result)
except Exception as e:
    log("EXC", repr(e))
try:
    spectrum.stop()
except Exception:
    pass
LOG.close()
print("DEMO_DONE")
