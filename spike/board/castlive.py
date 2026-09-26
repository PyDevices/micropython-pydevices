"""Cast the panel live: the hardware encoder reads the MIPI-DSI framebuffer through
the PPA (RGB565 to packed YUV420, centred in a 1280x720 canvas), tsmux packetizes,
RTP goes to the receiver. Spike code: everything runs in the caller's poll loop.
"""
import socket, time, math
import h264enc
from tsmux import TsMux, RtpOut, PCR_LEAD


def lpcm_block(amplitude=0, hz=500):
    """One 10 ms block of 48 kHz stereo 16-bit big-endian LPCM: silence, or a
    tone whose period divides 10 ms so blocks repeat without a click."""
    b = bytearray(1920)
    if amplitude:
        for i in range(480):
            v = int(amplitude * math.sin(2 * math.pi * hz * i / 48000))
            b[4 * i] = (v >> 8) & 0xFF
            b[4 * i + 1] = v & 0xFF
            b[4 * i + 2] = (v >> 8) & 0xFF
            b[4 * i + 3] = v & 0xFF
    return bytes(b)


class LiveStreamer:
    def __init__(self, dst_ip, dst_port, server_port, fb, fps=30, bitrate=3_000_000,
                 canvas=(1280, 720), scene=None, seconds=60, log=print, size=None, audio=None):
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
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)  # rebind at once on a back-to-back start
        self.sock.bind(("0.0.0.0", server_port))
        self.rtp = RtpOut(self.sock, (dst_ip, dst_port))
        # audio: None; "silence"; a tone amplitude (16 is -66 dBFS inaudible,
        # ~4000 is a moderate, clearly audible level); (amplitude, hz); or a
        # callable returning one 1920-byte LPCM block per 10 ms (real audio).
        self.audio = audio
        self.mux = TsMux(audio="lpcm" if audio is not None else None)
        self._audio_cb = audio if callable(audio) else None
        if audio is None:
            self.pcm = None
        elif callable(audio):
            self.pcm = lpcm_block(0)          # placeholder; blocks come from the callable
        elif audio == "silence":
            self.pcm = lpcm_block(0)
        elif isinstance(audio, (tuple, list)):
            self.pcm = lpcm_block(int(audio[0]), int(audio[1]) if len(audio) > 1 else 500)
        else:
            self.pcm = lpcm_block(int(audio))
        self.apts = 0
        self.audio_blocks = 0
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
        if self.pcm is not None:
            # keep the LPCM track about 100 ms ahead of the picture, in 10 ms blocks
            if self.apts == 0:
                self.apts = pts
            while self.apts < pts + 9000:
                block = self._audio_cb() if self._audio_cb else self.pcm
                self.mux.lpcm(block, self.apts, self.rtp)
                self.apts += 900
                self.audio_blocks += 1
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
        if self.pcm is not None:
            desc = "callable source" if self._audio_cb else ("silence" if self.audio == "silence" else "tone %s" % (self.audio,))
            self.log("audio: %d LPCM blocks (%.1f s), %s" % (self.audio_blocks, self.audio_blocks / 100, desc))

    def force_idr(self):
        self.enc.force_idr()

    def close(self):
        self.enc.close()
        self.sock.close()
