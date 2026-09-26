# Exercise the roku_cast smart-home API: ECP controls, then a background cast
# started and stopped like an app would.
import sys, time
sys.path.insert(0, "/cast")
from board_config import fb, display_drv
from roku_cast import RokuScreen

TV = "192.168.1.129"
LOG = open("/cast/roku_api.log", "w")


def log(*a):
    line = " ".join(str(x) for x in a)
    print(line); LOG.write(line + "\n"); LOG.flush()


display_drv.fill(0x03E0)
display_drv.fill_rect(120, 200, 480, 320, 0x001F)


class Scene:
    def __init__(self): self.n = 0
    def step(self):
        self.n += 1
        x = 20 + (self.n * 7) % 600            # x<=620, +80<=700<720
        display_drv.fill_rect(0, 80, 720, 90, 0x03E0)
        display_drv.fill_rect(x, 80, 80, 80, 0xFFE0)


tv = RokuScreen(TV, name="PyDevices P4", log=log)
log("on:", tv.on())
# ECP controls, net-neutral so the TV ends where it started
log("volume_up:", tv.volume_up())
time.sleep_ms(400)
log("volume_down:", tv.volume_down())
log("mute (on):", tv.mute()); time.sleep_ms(400)
log("mute (off):", tv.mute())
# background cast with audio, then stop it like an app would
log("start_cast:", tv.start_cast(fb, scene=Scene(), seconds=120, audio=(4000, 700)))
log("is_casting right after start:", tv.is_casting())
time.sleep(16)
log("is_casting after 16 s:", tv.is_casting())
log("stop_cast:", tv.stop_cast())
log("is_casting after stop:", tv.is_casting())
LOG.close()
print("API_TEST_DONE")
