# Prove audio on the Roku: cast the panel to the 65" with an audible LPCM tone.
# 700 Hz (a whole number of cycles per 10 ms block, so no click), amplitude 4000
# (~-18 dBFS, a moderate, clearly audible level). A host-side mic recording taken
# during this cast is analysed for the 700 Hz peak.
import sys, time
sys.path.insert(0, "/cast")
from board_config import fb, display_drv
from roku_cast import RokuScreen

TV = "192.168.1.129"
HZ = 700
AMP = 4000
SECONDS = 40
LOG = open("/cast/roku_audio.log", "w")
T0 = time.ticks_ms()


def log(*a):
    line = "[%6d] " % time.ticks_diff(time.ticks_ms(), T0) + " ".join(str(x) for x in a)
    print(line)
    LOG.write(line + "\n"); LOG.flush()


display_drv.fill(0x0008)
display_drv.fill_rect(0, 0, 720, 90, 0x07E0)
display_drv.fill_rect(180, 240, 360, 240, 0xF800)


class Scene:
    def __init__(self):
        self.n = 0

    def step(self):
        self.n += 1
        x = 20 + (self.n * 9) % 600
        display_drv.fill_rect(0, 120, 720, 70, 0x0008)
        display_drv.fill_rect(x, 120, 70, 70, 0xFFFF)  # x<=620, +70<=690<720


tv = RokuScreen(TV, name="PyDevices P4", log=log)
log("was on:", tv.is_on(), "showing:", tv.now_playing())
tv.on()
log("casting %d s with a %d Hz tone, amplitude %d" % (SECONDS, HZ, AMP))
try:
    result = tv.cast(fb, scene=Scene(), seconds=SECONDS, bitrate=3_000_000, audio=(AMP, HZ))
    log("result:", result)
except Exception as e:
    log("EXC", repr(e))
LOG.close()
print("ROKU_AUDIO_DONE")
