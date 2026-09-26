# Listen for input over the back channel: cast the panel, offer every HID type
# the receiver lists, and log whatever it sends. The stimulus is whoever acts in
# the Wireless Display window on the laptop.
import sys
sys.path.insert(0, "/cast")
import time, gc
from micecast import Session
from castlive import LiveStreamer
from board_config import fb, display_drv

SINK = "192.168.1.143"
SECONDS = 50
LOG = open("/cast/uibc_test.log", "w")
T0 = time.ticks_ms()


def log(*a):
    line = "[%6d] " % time.ticks_diff(time.ticks_ms(), T0) + " ".join(str(x) for x in a)
    print(line)
    LOG.write(line + "\n")
    LOG.flush()


display_drv.fill(0x0010)
display_drv.fill_rect(200, 200, 320, 320, 0x07E0)
try:
    display_drv.show()
except Exception:
    pass


received = []


def on_input(kind, data):
    received.append((kind, data))


gc.collect()
s = Session(SINK, log=log, on_input=on_input)
s.hidc_caps = "Keyboard/USB, Mouse/USB, MultiTouch/USB, Gesture/USB, RemoteControl/USB"


def make(dst_ip, dst_port, server_port):
    return LiveStreamer(dst_ip, dst_port, server_port, fb, fps=30, bitrate=3_000_000, seconds=SECONDS, log=log)


try:
    result = s.run(make, seconds=SECONDS + 15, idle_after_done=2)
    log("result:", result, "received", len(received))
    for k, d in received[:40]:
        log("  ", k, d.hex() if isinstance(d, (bytes, bytearray)) else d)
except Exception as e:
    log("EXC", repr(e))
LOG.close()
print("UIBC_TEST_DONE")
