# roku_cast.py -- a Roku TV as a wireless display you can also control.
#
# Two halves, for a PyDevices smart-home app:
#   * Control, over ECP (the examples roku_engine): power, keys, launching
#     apps, reading what is on screen. Works from any host with a socket,
#     the P4 included.
#   * Cast, over Miracast-over-Infrastructure (micecast + castlive): mirror a
#     framebuffer to the TV. Needs a board with an H.264 encoder, so the P4.
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
    def cast(self, fb, scene=None, seconds=60, fps=30, bitrate=3_000_000, audio=None, stop=None):
        """Mirror `fb` to the TV until `seconds` elapse or `stop()` returns True.
        `audio`: None, "silence", a tone amplitude, (amplitude, hz), or a
        callable returning a 10 ms LPCM block. Blocks until the cast ends."""
        def make(dst_ip, dst_port, server_port):
            return LiveStreamer(dst_ip, dst_port, server_port, fb, fps=fps,
                                bitrate=bitrate, scene=scene, seconds=seconds,
                                log=self.log, audio=audio)
        gc.collect()
        s = Session(self.host, name=self.name, log=self.log)
        s.session_request = 0        # the Roku wants the Session Request first
        self._session = s
        return s.run(make, seconds=seconds, idle_after_done=2, stop=stop)

    # --- non-blocking cast, for a long-running app: start it, stop it later ---
    def start_cast(self, fb, scene=None, seconds=3600, fps=30, bitrate=3_000_000, audio=None):
        """Cast in a background thread. Returns False if one is already running.
        Spike-grade: Phase 1 of the roadmap moves this to a C task on core 0."""
        if getattr(self, "_casting", False):
            return False
        import _thread
        self._stop = False
        self._casting = True

        def run():
            try:
                self.cast(fb, scene=scene, seconds=seconds, fps=fps,
                          bitrate=bitrate, audio=audio, stop=lambda: self._stop)
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
