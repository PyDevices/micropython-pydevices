# Control the 65" over Wi-Fi, then cast the P4's screen to it. A smart-home
# sketch: make sure the TV is on, say what it was showing, then mirror a
# framebuffer scene onto it for a minute.
import sys, time
sys.path.insert(0, "/cast")
from board_config import fb, display_drv
from roku_cast import RokuScreen

TV = "192.168.1.129"      # the 65" TCL Roku TV
SECONDS = 60
LOG = open("/cast/roku_cast.log", "w")
T0 = time.ticks_ms()


def log(*a):
    line = "[%6d] " % time.ticks_diff(time.ticks_ms(), T0) + " ".join(str(x) for x in a)
    print(line)
    LOG.write(line + "\n"); LOG.flush()


# A scene the TV can show: a title band and a bouncing block on colour.
display_drv.fill(0x0010)
display_drv.fill_rect(0, 0, 720, 90, 0x001F)
display_drv.fill_rect(140, 220, 440, 280, 0x07FF)
display_drv.fill_rect(240, 300, 240, 120, 0xF81F)


class Scene:
    def __init__(self):
        self.n = 0

    def step(self):
        self.n += 1
        x = 40 + (self.n * 8) % 620
        display_drv.fill_rect(0, 110, 720, 60, 0x0010)
        display_drv.fill_rect(x, 110, 60, 60, 0xFFE0)


tv = RokuScreen(TV, name="PyDevices P4", log=log)
log("was on:", tv.is_on(), "showing:", tv.now_playing())
tv.on()
log("casting", SECONDS, "s")
try:
    result = tv.cast(fb, scene=Scene(), seconds=SECONDS, bitrate=3_000_000)
    log("result:", result)
    log("now showing:", tv.now_playing())
except Exception as e:
    log("EXC", repr(e))
LOG.close()
print("ROKU_CAST_DONE")
