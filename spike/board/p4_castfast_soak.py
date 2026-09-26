# Phase 1 soak: castif casts a moving 720x720 scene to the 65" for 30 minutes.
# Records the keyframe-request rate and whether the session ever drops.
import sys, time
sys.path.insert(0, "/cast")
from board_config import fb, display_drv
import castfast
from micecast import Session

SINK = "192.168.1.129"
SECONDS = 1800
LOG = open("/cast/soak.log", "w")
T0 = time.ticks_ms()
def log(*a):
    line = "[%6ds] " % (time.ticks_diff(time.ticks_ms(), T0) // 1000) + " ".join(str(x) for x in a)
    print(line); LOG.write(line + "\n"); LOG.flush()

# make sure Wi-Fi is up (running a script over mpftp can soft-reset past main.py)
try:
    import wifi, network
    if not network.WLAN(network.STA_IF).isconnected():
        log("connecting Wi-Fi...")
        wifi.connect_from_secrets()
    log("Wi-Fi", network.WLAN(network.STA_IF).ifconfig()[0])
except Exception as e:
    log("wifi error", e)

display_drv.fill(0x0008)
class Scene:
    def __init__(self): self.n = 0
    def step(self):
        self.n += 1
        x = (self.n * 5) % 640
        display_drv.fill_rect(0, 100, 720, 80, 0x0008)
        display_drv.fill_rect(x, 100, 70, 80, 0xFFE0)
scene = Scene()

class SoakStreamer(castfast.CastifStreamer):
    def pump(self, b):
        scene.step()                 # keep the picture moving (busy case, no skips)
        now = time.ticks_ms()
        if now - getattr(self, "_b", 0) > 60000:
            self._b = now
            st = self.cast.stats()
            log("frames %d fps %.1f sent %d stalls %d skipped %d" % (
                st["frames"], st["fps"]/1000.0, st["sent"], st["stalls"], st.get("skipped",0)))
        super().pump(b)

c = castfast.make_caster(720, 720, fps=30, bitrate=3_000_000)
sess = Session(SINK, name="PyDevices P4", log=log)
sess.session_request = 0
def make(ip, dp, sp):
    s = SoakStreamer(c, fb, ip, dp, sp, SECONDS, log)
    return s
try:
    r = sess.run(make, seconds=SECONDS, idle_after_done=2)
    log("SOAK RESULT", r, "idr_requests", sess.idr_requests, "final", c.stats())
except Exception as e:
    sys.print_exception(e); log("SOAK EXC", repr(e))
finally:
    c.close()
LOG.close(); print("SOAK_DONE")
