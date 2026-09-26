# Play a stream on a Roku over ECP ("Play on Roku", channel 15985) using the
# examples engine directly, no UI. Run from Windows Python or the toolbox:
#   python roku_play.py [URL] [--restore APPID]   (default URL: a public HLS test stream)
# It takes over the TV screen; --restore relaunches an app (837 = YouTube) at the end.
# The volume is untouched.
import sys, time
sys.path.insert(0, "/home/brad/gh/pydevices/pydevices-examples/lib/examples/roku_remote")
import roku_engine as R
from urllib.parse import quote

TV = "192.168.1.129"
args = [a for a in sys.argv[1:] if not a.startswith("--")]
restore = None
if "--restore" in sys.argv:
    restore = sys.argv[sys.argv.index("--restore") + 1]
url = args[0] if args else "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"

e = R.RokuEngine(); e.set_host(TV); e.connect()
print("active:", e.query_active_app())
print("player:", str(e.query_media_player())[:200])
apps = e.query_apps()
print("play-on-roku listed:", [a for a in apps if "15985" in str(a) or "Play on" in str(a)][:3], "of", len(apps), "apps")
fmt = "hls" if ".m3u8" in url else "mp4"
cmd = "http://%s:8060/input/15985?t=v&u=%s&videoFormat=%s&videoName=%s" % (TV, quote(url, safe=""), fmt, quote("PyDevices test"))
print("POST", cmd[:140])
print("reply:", str(R.http_request("POST", cmd))[:200])
for i in range(4):
    time.sleep(5)
    print("t+%2ds active:" % (5 * (i + 1)), e.query_active_app(), "| player:", str(e.query_media_player())[:220])
if restore:
    print("restore:", restore, e.launch(restore) if hasattr(e, "launch") else R.http_request("POST", "http://%s:8060/launch/%s" % (TV, restore)))
