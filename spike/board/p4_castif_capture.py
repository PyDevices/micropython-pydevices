# p4_castif_capture.py -- the Phase 2 instrument's sender side: castif with
# audio, straight to a PC running spike/tools/rtp_capture.py (no MICE/RTSP, no
# sink), a moving scene and the melody through the wired speaker. The capture
# is analysed with spike/tools/ts_timing.py.
import sys, time
sys.path.insert(0, "/cast")
from board_config import fb, display_drv
from pumpcast import build_melody, BLOCK_BYTES
import castfast

PC = "192.168.1.143"
PORT = 5004
SECONDS = 60
BITRATE = 3_000_000
LOG = open("/cast/castif_capture.log", "w")
T0 = time.ticks_ms()


def log(*a):
    line = "[%6d] " % time.ticks_diff(time.ticks_ms(), T0) + " ".join(str(x) for x in a)
    print(line); LOG.write(line + "\n"); LOG.flush()


try:
    import wifi, network
    w = network.WLAN(network.STA_IF)
    if not w.isconnected():
        wifi.connect_from_secrets()
    log("Wi-Fi", w.ifconfig()[0], "rssi", w.status("rssi"))
except Exception as e:
    log("wifi error", e)

display_drv.fill(0x0006)
display_drv.fill_rect(0, 0, 720, 90, 0xF81F)
display_drv.show()


class Melody:
    def __init__(self):
        self.buf, self.frames = build_melody()
        b = self.buf
        for i in range(0, len(b), 2):
            b[i], b[i + 1] = b[i + 1], b[i]
        self.mv = memoryview(self.buf); self.total = self.frames * 4; self.pos = 0

    def __call__(self):
        end = self.pos + BLOCK_BYTES
        if end <= self.total:
            b = bytes(self.mv[self.pos:end]); self.pos = 0 if end == self.total else end
        else:
            rest = end - self.total
            b = bytes(self.mv[self.pos:self.total]) + bytes(self.mv[0:rest]); self.pos = rest
        return b


feed = castfast.PumpFeed(Melody(), log=log)
cast = castfast.make_caster(720, 720, fps=30, bitrate=BITRATE, audio=True)
feed.cast = cast
cast.start(fb, PC, PORT, 15550)
log("castif -> %s:%d, %d bps, audio on" % (PC, PORT, BITRATE))
t0 = time.ticks_ms(); n = 0; beat = 0
try:
    while time.ticks_diff(time.ticks_ms(), t0) < SECONDS * 1000:
        feed.pump()
        n += 1
        x = 20 + (n * 8) % 600
        display_drv.fill_rect(0, 110, 720, 80, 0x0006)
        display_drv.fill_rect(x, 110, 80, 80, 0xFFE0)
        display_drv.flush_rect(0, 110, 720, 80)
        if time.ticks_diff(time.ticks_ms(), t0) // 3000 > beat:
            beat = time.ticks_diff(time.ticks_ms(), t0) // 3000
            s = cast.stats()
            log("castif: %d f %.1f fps %d sent %d stalls | fed %d muxed %d level %d under %d drift %d ins %d drop %d skipped %d ringfull %d rssi %s" % (
                s["frames"], s["fps"] / 1000.0, s["sent"], s["stalls"], s["audio_fed"], s["audio_muxed"], s["audio_level"],
                s["audio_underruns"], s["audio_drift_ms"], s["audio_inserted"], s["audio_dropped"], feed.skipped, feed.full,
                w.status("rssi")))
        time.sleep_ms(20)
finally:
    s = cast.stats()
    cast.stop(); cast.close(); feed.close()
    log("done: %d frames, fed %d muxed %d under %d ins %d drop %d skipped %d" % (
        s["frames"], s["audio_fed"], s["audio_muxed"], s["audio_underruns"], s["audio_inserted"], s["audio_dropped"], feed.skipped))
LOG.close()
print("CASTIF_CAPTURE_DONE")
