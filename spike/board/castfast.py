# castfast.py -- cast with the castif C task (Phases 1 and 2).
#
# Same MICE + RTSP session as micecast, but on PLAY the streaming loop is the
# castif FreeRTOS task on core 0 instead of a Python pump: the Python side only
# answers RTSP keepalives, forwards keyframe requests, steps an optional scene
# and keeps the sound moving. Sound (Phase 2): a PumpFeed keeps the audio
# pump's stream topped up from a block source and hands the cast the same
# 10 ms blocks; the task muxes them as LPCM on the video's clock.
#
#   import castfast
#   castfast.cast(fb, "192.168.1.129", 720, 720, seconds=60)   # to a Roku, silent
#   castfast.cast(fb, "192.168.1.129", 720, 720, audio=feed)   # feed = castfast.PumpFeed(source)
#
# The sink is chosen the same way as roku_cast: session_request=0 for the Roku,
# None for Windows.
import sys, time

sys.path.insert(0, "/cast")
from micecast import Session
import castif


BLOCK_BYTES = 1920          # 10 ms of 48 kHz stereo s16


class PumpFeed:
    """The one producer for both listeners: the P4's speaker (the pump) and the cast.

    ``source()`` returns the next 10 ms block: 1920 bytes of 48 kHz stereo
    16-bit little-endian PCM, the pump's own format; the cast byte-swaps to
    LPCM on the wire. Production is paced by the wall clock, one block per
    10 ms plus a small lead, because the pump's stream ring never pushes back
    on this board (measured 2026-09-27: a 320 ms ring drained in 1.7 ms, and
    a feeder paced on ``space()`` ran at 1.5x real time). A stall longer than
    the lead is skipped, not caught up: what was not produced in time is
    silence at the sink, never a burst.
    """

    def __init__(self, source, stream=None, lead=8, log=print):
        self.source = source
        self.stream = stream
        self.cast = None
        self.lead = lead            # blocks kept ahead of the clock (80 ms)
        self.t0 = None
        self.n = 0                  # blocks produced since t0
        self.fed = 0
        self.full = 0
        self.skipped = 0            # blocks a stall cost
        self.log = log
        if stream is None:
            try:
                from audiodev import pump, AudioFormat
                self.stream = pump.attach_stream(AudioFormat(48000, 2, 16), capacity=32)
                log("pumpfeed: pump stream attached (%d bytes free)" % self.stream.space())
            except Exception as e:
                log("pumpfeed: no pump stream (%r); the cast gets the audio, the speaker does not" % (e,))
                self.stream = None

    def pump(self, limit=12):
        now = time.ticks_ms()
        if self.t0 is None:
            self.t0 = now
        want = time.ticks_diff(now, self.t0) // 10 + self.lead
        due = want - self.n
        if due > self.lead + limit:          # a stall: skip the lost time
            self.skipped += due - self.lead
            self.n = want - self.lead
            due = self.lead
        n = 0
        s = self.stream
        c = self.cast
        while n < due and n < limit:
            b = self.source()
            if s is not None and s.space() >= BLOCK_BYTES:
                s.write(b)
            if c is not None:
                if c.feed_audio(b):
                    self.fed += 1
                else:
                    self.full += 1
            self.n += 1
            n += 1
        return n

    def close(self):
        if self.stream is not None:
            try:
                self.stream.deinit()
            except Exception:
                pass
            self.stream = None


class CastifStreamer:
    """Adapts the castif task to the micecast streamer interface (pump/done/
    force_idr/close) so micecast.Session drives it unchanged."""

    def __init__(self, cast, fb, dst_ip, dst_port, server_port, seconds, log, scene=None, audio=None):
        self.cast = cast
        self.log = log
        self.seconds = seconds
        self.scene = scene          # stepped from pump(): input polling and drawing stay in Python
        self.audio = audio          # a PumpFeed, or None for a silent cast
        if audio is not None:
            audio.cast = cast
        self.t0 = time.ticks_ms()
        self.done = False
        self.idle_poll = True   # the C task streams; the Session loop can block, not spin
        cast.start(fb, dst_ip, dst_port, server_port)
        log("castif task -> %s:%d from :%d" % (dst_ip, dst_port, server_port))

    def pump(self, budget_us):
        # the C task does the streaming; here we step the scene (if any), enforce
        # the duration and surface a stats line every few seconds
        if self.audio is not None:
            self.audio.pump()
        if self.scene:
            self.scene.step()
        now = time.ticks_ms()
        if now - getattr(self, "_beat", 0) > 3000:
            self._beat = now
            s = self.cast.stats()
            line = "castif: %d frames, %.1f fps, %d sent, %d stalls, enc %d us, ppa %d us, mux %d us, %d B/f" % (
                s["frames"], s["fps"] / 1000.0, s["sent"], s["stalls"],
                s["enc_us"], s["ppa_us"], s["mux_us"], s["length"])
            if self.audio is not None and "audio_fed" in s:
                line += " | audio fed %d muxed %d level %d underruns %d drift %d ms ins %d drop %d ringfull %d" % (
                    s["audio_fed"], s["audio_muxed"], s["audio_level"], s["audio_underruns"],
                    s["audio_drift_ms"], s["audio_inserted"], s["audio_dropped"], self.audio.full)
                line += " skipped %d" % self.audio.skipped
            self.log(line)
        if self.seconds and time.ticks_diff(now, self.t0) > self.seconds * 1000:
            self.done = True

    def force_idr(self):
        self.cast.force_idr()

    def close(self):
        s = self.cast.stats()
        self.cast.stop()
        self.log("castif stopped: %d frames, %.1f fps, %d sent, %d stalls" % (
            s["frames"], s["fps"] / 1000.0, s["sent"], s["stalls"]))


def make_caster(w, h, canvas=(1280, 720), fps=30, bitrate=3_000_000, audio=False):
    """A reusable castif.Cast (the encoder + PPA are set up once); audio=True adds the LPCM ring."""
    if audio:
        return castif.Cast(w, h, canvas_w=canvas[0], canvas_h=canvas[1], fps=fps, bitrate=bitrate, audio=True)
    return castif.Cast(w, h, canvas_w=canvas[0], canvas_h=canvas[1], fps=fps, bitrate=bitrate)


def cast(fb, sink_ip, w, h, canvas=(1280, 720), fps=30, bitrate=3_000_000,
         seconds=60, session_request=0, name="PyDevices P4", log=print, cast_obj=None,
         scene=None, audio=None):
    c = cast_obj or make_caster(w, h, canvas=canvas, fps=fps, bitrate=bitrate, audio=audio is not None)

    def make(dst_ip, dst_port, server_port):
        return CastifStreamer(c, fb, dst_ip, dst_port, server_port, seconds, log, scene=scene, audio=audio)

    s = Session(sink_ip, name=name, log=log)
    s.session_request = session_request
    try:
        return s.run(make, seconds=seconds, idle_after_done=2)
    finally:
        if cast_obj is None:
            c.close()
