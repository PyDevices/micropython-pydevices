# castfast.py -- cast with the castif C task (Phase 1).
#
# Same MICE + RTSP session as micecast, but on PLAY the streaming loop is the
# castif FreeRTOS task on core 0 instead of a Python pump: the Python side only
# answers RTSP keepalives and forwards keyframe requests. Video only for now
# (Phase 1); sound is Phase 2.
#
#   import castfast
#   castfast.cast(fb, "192.168.1.129", 720, 720, seconds=60)   # to a Roku
#
# The sink is chosen the same way as roku_cast: session_request=0 for the Roku,
# None for Windows.
import sys, time

sys.path.insert(0, "/cast")
from micecast import Session
import castif


class CastifStreamer:
    """Adapts the castif task to the micecast streamer interface (pump/done/
    force_idr/close) so micecast.Session drives it unchanged."""

    def __init__(self, cast, fb, dst_ip, dst_port, server_port, seconds, log):
        self.cast = cast
        self.log = log
        self.seconds = seconds
        self.t0 = time.ticks_ms()
        self.done = False
        cast.start(fb, dst_ip, dst_port, server_port)
        log("castif task -> %s:%d from :%d" % (dst_ip, dst_port, server_port))

    def pump(self, budget_us):
        # the C task does the streaming; here we only enforce the duration and
        # surface a stats line every few seconds
        now = time.ticks_ms()
        if now - getattr(self, "_beat", 0) > 3000:
            self._beat = now
            s = self.cast.stats()
            self.log("castif: %d frames, %.1f fps, %d sent, %d stalls, enc %d us, ppa %d us, mux %d us, %d B/f" % (
                s["frames"], s["fps"] / 1000.0, s["sent"], s["stalls"],
                s["enc_us"], s["ppa_us"], s["mux_us"], s["length"]))
        if self.seconds and time.ticks_diff(now, self.t0) > self.seconds * 1000:
            self.done = True

    def force_idr(self):
        self.cast.force_idr()

    def close(self):
        s = self.cast.stats()
        self.cast.stop()
        self.log("castif stopped: %d frames, %.1f fps, %d sent, %d stalls" % (
            s["frames"], s["fps"] / 1000.0, s["sent"], s["stalls"]))


def make_caster(w, h, canvas=(1280, 720), fps=30, bitrate=3_000_000):
    """A reusable castif.Cast (the encoder + PPA are set up once)."""
    return castif.Cast(w, h, canvas_w=canvas[0], canvas_h=canvas[1], fps=fps, bitrate=bitrate)


def cast(fb, sink_ip, w, h, canvas=(1280, 720), fps=30, bitrate=3_000_000,
         seconds=60, session_request=0, name="PyDevices P4", log=print, cast_obj=None):
    c = cast_obj or make_caster(w, h, canvas=canvas, fps=fps, bitrate=bitrate)

    def make(dst_ip, dst_port, server_port):
        return CastifStreamer(c, fb, dst_ip, dst_port, server_port, seconds, log)

    s = Session(sink_ip, name=name, log=log)
    s.session_request = session_request
    try:
        return s.run(make, seconds=seconds, idle_after_done=2)
    finally:
        if cast_obj is None:
            c.close()
