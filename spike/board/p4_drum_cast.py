# p4_drum_cast.py -- the Phase 2 gate: the drum machine on the P4, its sound
# on the cast. The app is the stock pydevices-examples drum machine (LVGL, its
# groove on the audio pump's clock); the cast takes the pump's OUTPUT through
# castfast.TapFeed, so what the P4 plays is what the sink gets.
#
#   MODE = "tv": cast to the 65" for a listen (hits with the bars).
#   MODE = "capture": castif straight to the PC's rtp_capture.py for the
#   measured part (10 minutes: drift, dropouts, A/V offset).
#
# The drum machine's module runs its app loop at import, so the cast is
# started first (a thread) and the app then owns the main thread.
import sys, time
sys.path.insert(0, "/cast")
sys.path.insert(0, "/lib/examples")

MODE = "tv"
try:                       # /cast/drum_mode.txt overrides: "tv" or "capture"
    MODE = open("/cast/drum_mode.txt").read().strip() or MODE
except OSError:
    pass
TV = "192.168.1.129"
PC = "192.168.1.143"
SECONDS = 150 if MODE == "tv" else 630
LOG = open("/cast/drum_cast.log", "w")
T0 = time.ticks_ms()


def log(*a):
    line = "[%6d] " % time.ticks_diff(time.ticks_ms(), T0) + " ".join(str(x) for x in a)
    print(line); LOG.write(line + "\n"); LOG.flush()


try:
    import wifi, network
    w = network.WLAN(network.STA_IF)
    if not w.isconnected():
        wifi.connect_from_secrets()
    w.config(pm=network.WLAN.PM_NONE)
    log("Wi-Fi", w.ifconfig()[0], "rssi", w.status("rssi"))
except Exception as e:
    log("wifi error", e)

from board_config import fb
import castfast

# the drum machine opens board_peripherals.audio_out(latency="low"), which on
# the P4 is 24 kHz mono; the feed converts it to the cast's 48 kHz stereo
feed = castfast.TapFeed(rate=24000, channels=1, log=log)

if MODE == "tv":
    from roku_cast import RokuScreen
    tv = RokuScreen(TV, name="PyDevices Drums", log=log)
    tv.on()
    ok = tv.start_cast(fb, audio=feed, seconds=SECONDS, skip_ms=0)   # every frame: the step light is small
    log("cast thread started:", ok)
else:
    import _thread
    # a scheduled LVGL callback (the touch read) can run on whichever thread
    # is sleeping, and the default 5 KB thread stack overflows in it
    _thread.stack_size(32 * 1024)
    cast = castfast.make_caster(720, 720, fps=30, bitrate=3_000_000, audio=True)
    feed.cast = cast
    cast.start(fb, PC, 5004, 15550)
    cast.set_skip(0)       # after start(): start() puts the 500 ms floor back
    log("castif -> %s:5004 with the pump tap, %d s" % (PC, SECONDS))

    def pumper():
        t0 = time.ticks_ms(); beat = 0
        try:
            while time.ticks_diff(time.ticks_ms(), t0) < SECONDS * 1000:
                feed.pump()
                el = time.ticks_diff(time.ticks_ms(), t0)
                if el // 5000 > beat:
                    beat = el // 5000
                    s = cast.stats()
                    log("castif: %d f %.1f fps %d sent %d stalls | fed %d muxed %d level %d under %d drift %d ins %d drop %d lapped %d ringfull %d in %d rssi %s" % (
                        s["frames"], s["fps"] / 1000.0, s["sent"], s["stalls"], s["audio_fed"], s["audio_muxed"],
                        s["audio_level"], s["audio_underruns"], s["audio_drift_ms"], s["audio_inserted"],
                        s["audio_dropped"], feed.lapped, feed.full, feed.in_bytes, w.status("rssi")))
                time.sleep_ms(25)
        finally:
            s = cast.stats()
            cast.stop()
            log("done: %d frames, fed %d muxed %d under %d ins %d drop %d lapped %d" % (
                s["frames"], s["audio_fed"], s["audio_muxed"], s["audio_underruns"], s["audio_inserted"],
                s["audio_dropped"], feed.lapped))
            LOG.flush()

    _thread.start_new_thread(pumper, ())

# In capture mode nobody is at the panel, so PLAY is pressed for the run:
# a thread waits for the app object, then schedules the press onto the main
# thread (LVGL is not thread-safe).
if MODE == "capture":
    import _thread, micropython

    def press_play(_):
        import lvgl as lv
        dm = sys.modules["drum_machine.drum_machine"].machine
        dm.play_btn.add_state(lv.STATE.CHECKED)
        dm._on_play(None)
        log("PLAY pressed")

    def autoplay():
        while getattr(sys.modules.get("drum_machine.drum_machine"), "machine", None) is None:
            time.sleep_ms(200)
        time.sleep_ms(3000)
        micropython.schedule(press_play, None)

    _thread.start_new_thread(autoplay, ())

log("starting the drum machine")
import drum_machine    # noqa: E402  runs app.run(): the LVGL app owns the main thread from here
