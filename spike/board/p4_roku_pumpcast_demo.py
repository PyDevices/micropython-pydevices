# The smart-home audio path: a melody plays through the P4's audio pump and the
# same audio is cast to the 65". The cast's audio callback is PumpCast.block,
# which writes each 10 ms block into the pump (speaker) and returns it (TV).
import sys, time
sys.path.insert(0, "/cast")
from board_config import fb, display_drv
from roku_cast import RokuScreen
from pumpcast import PumpCast

TV = "192.168.1.129"
SECONDS = 30
LOG = open("/cast/roku_pumpcast.log", "w")
T0 = time.ticks_ms()


def log(*a):
    line = "[%6d] " % time.ticks_diff(time.ticks_ms(), T0) + " ".join(str(x) for x in a)
    print(line); LOG.write(line + "\n"); LOG.flush()


display_drv.fill(0x0006)
display_drv.fill_rect(0, 0, 720, 90, 0xF81F)
display_drv.fill_rect(160, 240, 400, 240, 0x07E0)


class Scene:
    def __init__(self): self.n = 0
    def step(self):
        self.n += 1
        x = 20 + (self.n * 8) % 600            # x<=620, +80<=700<720
        display_drv.fill_rect(0, 110, 720, 80, 0x0006)
        display_drv.fill_rect(x, 110, 80, 80, 0xFFE0)


log("building melody...")
pc = PumpCast(log=log)
log("melody: %d blocks, %d bytes" % (pc.frames // 480, pc.total))
tv = RokuScreen(TV, name="PyDevices P4", log=log)
tv.on()
log("casting the melody %d s" % SECONDS)
try:
    result = tv.cast(fb, scene=Scene(), seconds=SECONDS, audio=pc.block)
    log("result:", result)
except Exception as e:
    log("EXC", repr(e))
finally:
    pc.close()
LOG.close()
print("PUMPCAST_DONE")
