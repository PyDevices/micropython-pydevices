"""Cast the panel live: the hardware encoder reads the MIPI-DSI framebuffer through
the PPA (RGB565 to packed YUV420, centred in a 1280x720 canvas), tsmux packetizes,
RTP goes to the receiver. Spike code: everything runs in the caller's poll loop.
"""
import socket, time
import h264enc
from tsmux import TsMux, RtpOut, PCR_LEAD


class LiveStreamer:
    def __init__(self, dst_ip, dst_port, server_port, fb, fps=30, bitrate=3_000_000,
                 canvas=(1280, 720), scene=None, seconds=60, log=print, size=None):
        """fb: any RGB565 buffer (the panel's Display, or a memoryview) with width and
        height, or size=(w, h) for a plain buffer."""
        self.fb = fb
        self.fps = fps
        self.log = log
        w, h = size if size else (fb.width, fb.height)
        cw, ch = canvas if canvas else (w, h)
        self.enc = h264enc.Encoder(w, h, fps=fps, gop=fps, bitrate=bitrate, canvas_w=cw, canvas_h=ch)
        self.out = bytearray(cw * ch // 2)
        self.mv = memoryview(self.out)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", server_port))
        self.rtp = RtpOut(self.sock, (dst_ip, dst_port))
        self.mux = TsMux()
        self.scene = scene
        self.t0 = time.ticks_us()
        self.next_us = 0
        self.frames = 0
        self.enc_us = 0
        self.ppa_us = 0
        self.mux_us = 0
        self.draw_us = 0
        self.bytes = 0
        self.max_enc = 0
        self.done = False
        self.end_us = seconds * 1000000
        log("live: %dx%d in %dx%d at %d fps, %d bps, to %s:%d from %d" % (w, h, cw, ch, fps, bitrate, dst_ip, dst_port, server_port))

    def pump(self, budget_us):
        self.pumps = getattr(self, "pumps", 0) + 1
        now = time.ticks_diff(time.ticks_us(), self.t0)
        if now - getattr(self, "last_beat", 0) > 3000000:
            self.last_beat = now
            self.log("beat: %d pumps, %d frames, next in %d us, %d stalls" % (self.pumps, self.frames, self.next_us - now, self.rtp.stalls))
        if now >= self.end_us:
            self.done = True
            self.report(now)
            return
        if now < self.next_us:
            return
        self.next_us += 1000000 // self.fps
        if now - self.next_us > 100000:
            self.next_us = now
        t = time.ticks_us()
        if self.scene:
            self.scene.step()
        t1 = time.ticks_us()
        n = self.enc.encode(self.fb, self.out)
        t2 = time.ticks_us()
        if self.frames == 0:
            self.log("first frame: %d bytes, type %d, convert %d us, encode %d us" % (n, self.enc.frame_type, self.enc.ppa_us, self.enc.us))
        pts = 90000 + now * 9 // 100
        key = self.enc.frame_type == 0
        if key:
            self.mux.tables(self.rtp)
        self.mux.video(self.mv[:n], pts, self.rtp, aud=True, key=key)
        self.rtp.ts90 = pts - PCR_LEAD
        self.rtp.flush()
        t3 = time.ticks_us()
        if self.frames < 6:
            self.log("frame %d steps: draw %d, encode %d (convert %d), mux+send %d us" % (self.frames, time.ticks_diff(t1, t), time.ticks_diff(t2, t1), self.enc.ppa_us, time.ticks_diff(t3, t2)))
        self.frames += 1
        self.draw_us += time.ticks_diff(t1, t)
        e = time.ticks_diff(t2, t1)
        self.enc_us += e
        self.ppa_us += self.enc.ppa_us
        if e > self.max_enc:
            self.max_enc = e
        self.mux_us += time.ticks_diff(t3, t2)
        self.bytes += n
        if self.frames % 60 == 0:
            self.report(now)

    def report(self, now):
        f = self.frames or 1
        self.log("frames %d in %.1f s = %.1f fps; draw %d us, convert %d us + encode %d us (max %d), mux+send %d us, %d bytes/frame = %d kbit/s, %d RTP, %d stalls" % (
            self.frames, now / 1e6, self.frames * 1e6 / max(now, 1), self.draw_us // f, self.ppa_us // f, (self.enc_us - self.ppa_us) // f, self.max_enc,
            self.mux_us // f, self.bytes // f, self.bytes * 8 * 1000 // max(now, 1), self.rtp.sent, self.rtp.stalls))

    def force_idr(self):
        self.enc.force_idr()

    def close(self):
        self.enc.close()
        self.sock.close()
