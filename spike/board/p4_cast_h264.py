# Cast a raw H.264 file (/cast/test.h264 + .idx of access-unit sizes) at 30 fps
# through tsmux + RTP to the Windows receiver: proves the on-board packetizer
# against the real sink before the encoder exists.
import sys
sys.path.insert(0, "/cast")
import socket, struct, time, os
from micecast import Session
from tsmux import TsMux, RtpOut

SINK = "192.168.1.143"
FPS = 30
AUDIO = False
RAI = True
LOOPS = 3
LOG = open("/cast/cast_h264.log", "w")
T0 = time.ticks_ms()


def log(*a):
    line = "[%6d] " % time.ticks_diff(time.ticks_ms(), T0) + " ".join(str(x) for x in a)
    print(line)
    LOG.write(line + "\n")
    LOG.flush()


class FileStreamer:
    def __init__(self, dst_ip, dst_port, server_port):
        self.f = open("/cast/test.h264", "rb")
        idx = open("/cast/test.h264.idx", "rb").read()
        self.sizes = [struct.unpack_from("<I", idx, 4 * i)[0] for i in range(len(idx) // 4)]
        self.au = bytearray(max(self.sizes))
        self.mv = memoryview(self.au)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", server_port))
        self.out = RtpOut(self.sock, (dst_ip, dst_port))
        self.mux = TsMux(audio="lpcm" if AUDIO else None)
        self.silence = bytes(1920)      # 480 stereo 16-bit frames: 10 ms at 48 kHz
        self.apts = 0
        self.i = 0
        self.t0 = time.ticks_us()
        self.done = False
        self.mux_us = 0
        self.mux_max = 0
        self.late = 0
        log("stream: %d frames to %s:%d from port %d" % (len(self.sizes), dst_ip, dst_port, server_port))

    def pump(self, budget_us):
        if self.done:
            return
        now = time.ticks_diff(time.ticks_us(), self.t0)
        due = self.i * 1000000 // FPS
        if now < due:
            return
        if now - due > 40000:
            self.late += 1
        k = self.i % len(self.sizes)
        if k == 0 and self.i:
            self.f.seek(0)
        n = self.sizes[k]
        self.f.readinto(self.mv[:n])
        t = time.ticks_us()
        if self.i % FPS == 0:
            self.mux.tables(self.out)
        pts = 90000 + self.i * (90000 // FPS)
        self.mux.video(self.mv[:n], pts, self.out, key=RAI and self.i % FPS == 0)
        # keep a silent LPCM track a little ahead of the video
        while AUDIO and self.apts < pts + 9000:
            self.mux.lpcm(self.silence, 90000 + self.apts, self.out)
            self.apts += 900
        self.out.ts90 = pts - 36000     # the RTP clock is the sender's clock, i.e. the PCR
        self.out.flush()
        dt = time.ticks_diff(time.ticks_us(), t)
        self.mux_us += dt
        if dt > self.mux_max:
            self.mux_max = dt
        self.i += 1
        if self.i % 60 == 0:
            log("frame %d: mux avg %d us max %d us, %d RTP, %d stalls, late %d" % (self.i, self.mux_us // self.i, self.mux_max, self.out.sent, self.out.stalls, self.late))
        if self.i >= len(self.sizes) * LOOPS:
            self.done = True
            log("stream: done, %d frames in %.1f s, mux avg %d us max %d, %d RTP packets, %d stalls, %d late" % (self.i, now / 1e6, self.mux_us // self.i, self.mux_max, self.out.sent, self.out.stalls, self.late))

    def close(self):
        self.f.close()
        self.sock.close()


s = Session(SINK, audio_m4="LPCM 00000002 00" if AUDIO else "AAC 00000001 00", log=log)
try:
    result = s.run(FileStreamer, seconds=90, idle_after_done=4)
    log("result:", result)
except Exception as e:
    log("EXC", repr(e))
LOG.close()
print("CAST_DONE")
