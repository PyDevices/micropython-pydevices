# roku_cast.py -- a Roku TV as a wireless display you can also control.
#
# Two halves, for a PyDevices smart-home app:
#   * Control, over ECP (the examples roku_engine): power, keys, launching
#     apps, reading what is on screen. Works from any host with a socket,
#     the P4 included.
#   * Cast, over Miracast-over-Infrastructure (micecast + the castif C task,
#     or the spike's Python castlive loop): mirror a framebuffer to the TV.
#     Needs a board with an H.264 encoder, so the P4.
#
# The Roku needs the MICE Session Request first, and micecast now answers the
# sink's own OPTIONS before asking for its capabilities (an early GET_PARAMETER
# earns a 455 from Roku OS, though Windows tolerates it).
import sys, time, gc

sys.path.insert(0, "/cast")                      # micecast, castlive
sys.path.insert(0, "/lib/examples/roku_remote")  # roku_engine (ECP)

from micecast import Session
from castlive import LiveStreamer

try:
    import roku_engine
except ImportError:
    roku_engine = None


class RokuScreen:
    def __init__(self, host, name="PyDevices", log=print):
        self.host = host
        self.name = name
        self.log = log
        self.eng = None
        if roku_engine is not None:
            self.eng = roku_engine.RokuEngine()
            self.eng.set_host(host)
            self.eng.connect()

    # --- control (ECP) ---
    def is_on(self):
        return self.eng.power_is_on() if self.eng else None

    def on(self):
        if self.eng and not self.eng.power_is_on():
            self.log("roku: powering on")
            self.eng.press("PowerOn")
            time.sleep(2)
        return self.is_on()

    def press(self, key):
        return self.eng.press(key) if self.eng else None

    def launch(self, app_id, query=""):
        return self.eng.launch(app_id, query) if self.eng else None

    def now_playing(self):
        return self.eng.query_active_app() if self.eng else None

    def off(self):
        return self.press("PowerOff")

    def volume_up(self, n=1):
        r = None
        for _ in range(n):
            r = self.press("VolumeUp")
        return r

    def volume_down(self, n=1):
        r = None
        for _ in range(n):
            r = self.press("VolumeDown")
        return r

    def mute(self):
        return self.press("VolumeMute")

    def set_input(self, name):
        # Roku inputs launch like apps: "hdmi1".."hdmi4", "dtv", "av1".
        if not name.startswith("tvinput."):
            name = "tvinput." + name
        return self.launch(name)

    # --- cast (Miracast over Infrastructure) ---
    def cast(self, fb, scene=None, seconds=60, fps=30, bitrate=3_000_000, audio=None, stop=None,
             engine="castif", size=(720, 720)):
        """Mirror `fb` to the TV until `seconds` elapse or `stop()` returns True.
        Blocks until the cast ends.

        engine="castif" (default): the C task on core 0 streams; `audio` is a
        castfast.PumpFeed or a callable returning 10 ms blocks of 48 kHz
        stereo s16 little-endian PCM (the speaker's format).
        engine="python": the spike's Python loop; `audio` is None, "silence",
        a tone amplitude, (amplitude, hz), or a callable returning big-endian
        LPCM blocks."""
        if engine == "castif":
            return self._cast_castif(fb, scene, seconds, fps, bitrate, audio, stop, size)

        def make(dst_ip, dst_port, server_port):
            return LiveStreamer(dst_ip, dst_port, server_port, fb, fps=fps,
                                bitrate=bitrate, scene=scene, seconds=seconds,
                                log=self.log, audio=audio)
        gc.collect()
        s = Session(self.host, name=self.name, log=self.log)
        s.session_request = 0        # the Roku wants the Session Request first
        self._session = s
        return s.run(make, seconds=seconds, idle_after_done=2, stop=stop)

    def _cast_castif(self, fb, scene, seconds, fps, bitrate, audio, stop, size):
        import castfast
        w, h = size
        want_audio = audio is not None
        c = getattr(self, "_caster", None)
        # one encoder per board (esp_h264 is a singleton): keep it across casts,
        # rebuild only when the audio ring is wanted and it has none
        if c is None or getattr(self, "_caster_audio", False) != want_audio:
            if c is not None:
                c.close()
            c = self._caster = castfast.make_caster(w, h, fps=fps, bitrate=bitrate, audio=want_audio)
            self._caster_audio = want_audio
        feed = None
        if want_audio:
            feed = audio if isinstance(audio, castfast.PumpFeed) else castfast.PumpFeed(audio, log=self.log)

        def make(dst_ip, dst_port, server_port):
            return castfast.CastifStreamer(c, fb, dst_ip, dst_port, server_port, seconds, self.log,
                                           scene=scene, audio=feed)
        gc.collect()
        s = Session(self.host, name=self.name, log=self.log)
        s.session_request = 0
        self._session = s
        try:
            return s.run(make, seconds=seconds, idle_after_done=2, stop=stop)
        finally:
            if feed is not None and feed is not audio:
                feed.close()

    # --- non-blocking cast, for a long-running app: start it, stop it later ---
    def start_cast(self, fb, scene=None, seconds=3600, fps=30, bitrate=3_000_000, audio=None, engine="castif"):
        """Cast in a background thread. Returns False if one is already running."""
        if getattr(self, "_casting", False):
            return False
        import _thread
        self._stop = False
        self._casting = True

        def run():
            try:
                self.cast(fb, scene=scene, seconds=seconds, fps=fps,
                          bitrate=bitrate, audio=audio, stop=lambda: self._stop, engine=engine)
            except Exception as e:
                self.log("cast thread:", repr(e))
            finally:
                self._casting = False

        _thread.start_new_thread(run, ())
        return True

    def stop_cast(self):
        """Ask a background cast to stop; returns once it has, or after ~5 s."""
        self._stop = True
        for _ in range(50):
            if not getattr(self, "_casting", False):
                return True
            time.sleep_ms(100)
        return not self._casting

    def is_casting(self):
        return getattr(self, "_casting", False)
