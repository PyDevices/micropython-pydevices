# Phase 1 fps gate: castif casts 720x720 on core 0 while an app redraws the
# panel on core 1. Measure the app's rate cast vs uncast (must stay within 10 %)
# and castif's fps (must be >= 25).
import sys, time, _thread
sys.path.insert(0, "/cast")
from board_config import fb, display_drv
import castfast

SINK = "192.168.1.129"   # the 65" Roku
SECONDS = 25
W = H = 720
LOG = open("/cast/castfast_gate.log", "w")
def log(*a):
    s = " ".join(str(x) for x in a); print(s); LOG.write(s + "\n"); LOG.flush()

display_drv.fill(0x0008)
display_drv.fill_rect(140, 220, 440, 280, 0x07FF)
class App:
    def __init__(self): self.n = 0
    def draw(self):
        self.n += 1
        x = (self.n * 6) % 640
        display_drv.fill_rect(0, 90, 720, 80, 0x0008)
        display_drv.fill_rect(x, 90, 70, 80, 0xFFE0)

def run_app(secs):
    app = App(); t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < secs * 1000:
        app.draw()
    return app.n / secs

# 1) uncast baseline
log("uncast app rate...")
base = run_app(6)
log("uncast app fps: %.1f" % base)

# 2) cast on core 0 (RTSP session in a thread), app on the main thread
state = {"caster": None, "done": False, "result": None}
def session():
    try:
        state["caster"] = castfast.make_caster(W, H, fps=30, bitrate=3_000_000)
        state["result"] = castfast.cast(fb, SINK, W, H, fps=30, seconds=SECONDS,
                                        log=log, cast_obj=state["caster"])
    except Exception as e:
        log("session EXC", repr(e))
    state["done"] = True

_thread.start_new_thread(session, ())
time.sleep(14)   # let MICE + RTSP establish and castif start
log("casting; measuring app rate on core 1")
capp = run_app(8)
c = state["caster"]
s = c.stats() if c else {}
log("RESULT uncast_app_fps %.1f cast_app_fps %.1f (%.0f%% of uncast); castif_fps %.1f frames %d stalls %d" % (
    base, capp, 100.0 * capp / base if base else 0, s.get("fps", 0) / 1000.0, s.get("frames", 0), s.get("stalls", 0)))
# let the session finish
for _ in range(30):
    if state["done"]: break
    time.sleep(1)
LOG.close(); print("GATE_DONE")
