# p4_castfast_audio_demo.py -- Phase 2 ear check: the melody through the P4's
# pump AND the castif cast, one producer for both, the cast's audio muxed by the
# C task on the video's clock. The Python path stuttered with pitch wobble on
# the 65" (2026-09-27, Brad); this is the same material on the C path.
import sys, time
sys.path.insert(0, "/cast")
from board_config import fb, display_drv
from roku_cast import RokuScreen
from pumpcast import build_melody, BLOCK_BYTES
import castfast

TV = "192.168.1.129"
SECONDS = 60
LOG = open("/cast/castfast_audio.log", "w")
T0 = time.ticks_ms()


def log(*a):
    line = "[%6d] " % time.ticks_diff(time.ticks_ms(), T0) + " ".join(str(x) for x in a)
    print(line); LOG.write(line + "\n"); LOG.flush()


try:
    import wifi, network
    if not network.WLAN(network.STA_IF).isconnected():
        log("connecting Wi-Fi...")
        wifi.connect_from_secrets()
    log("Wi-Fi", network.WLAN(network.STA_IF).ifconfig()[0])
except Exception as e:
    log("wifi error", e)

display_drv.fill(0x0006)
display_drv.fill_rect(0, 0, 720, 90, 0xF81F)
display_drv.fill_rect(160, 240, 400, 240, 0x07E0)
display_drv.show()


class Scene:
    def __init__(self):
        self.n = 0
        self.t = time.ticks_ms()

    def step(self):
        now = time.ticks_ms()
        if time.ticks_diff(now, self.t) < 40:
            return
        self.t = now
        self.n += 1
        x = 20 + (self.n * 8) % 600
        display_drv.fill_rect(0, 110, 720, 80, 0x0006)
        display_drv.fill_rect(x, 110, 80, 80, 0xFFE0)
        display_drv.flush_rect(0, 110, 720, 80)


class Melody:
    def __init__(self):
        self.buf, self.frames = build_melody()
        # build_melody writes big-endian (the Python cast path's format); the
        # pump and the castif ring take little-endian, so swap once here
        b = self.buf
        for i in range(0, len(b), 2):
            b[i], b[i + 1] = b[i + 1], b[i]
        self.mv = memoryview(self.buf)
        self.total = self.frames * 4
        self.pos = 0

    def __call__(self):
        end = self.pos + BLOCK_BYTES
        if end <= self.total:
            b = bytes(self.mv[self.pos:end])
            self.pos = 0 if end == self.total else end
        else:
            rest = end - self.total
            b = bytes(self.mv[self.pos:self.total]) + bytes(self.mv[0:rest])
            self.pos = rest
        return b


log("building melody...")
mel = Melody()
log("melody: %d blocks" % (mel.frames // 480))
feed = castfast.PumpFeed(mel, log=log)
try:
    import board_peripherals as bp
    bp.audio_power(True, volume=85)      # the panel's listening level
    log("P4 codec at 85 %")
except Exception as e:
    log("codec volume not set:", e)
tv = RokuScreen(TV, name="PyDevices P4", log=log)
tv.on()
log("casting %d s with audio on the castif task" % SECONDS)
try:
    result = castfast.cast(fb, TV, 720, 720, seconds=SECONDS, session_request=0, log=log,
                           scene=Scene(), audio=feed)
    log("result:", result, "fed", feed.fed, "ring-full", feed.full)
except Exception as e:
    log("EXC", repr(e))
finally:
    feed.close()
LOG.close()
print("CASTFAST_AUDIO_DONE")
