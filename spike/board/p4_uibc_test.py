# Does the receiver forward real input over the back channel? The P4 becomes a
# USB mouse and keyboard on its native port, casts, and once the receiver has
# connected the UIBC socket, moves the laptop's own cursor, clicks, and types
# a letter; whatever comes back over UIBC is logged.
import sys
sys.path.insert(0, "/cast")
import time, gc
import usbif.auto
from micecast import Session
from castlive import LiveStreamer
from board_config import fb, display_drv

SINK = "192.168.1.143"
SECONDS = 45
LOG = open("/cast/uibc_test.log", "w")
T0 = time.ticks_ms()


def log(*a):
    line = "[%6d] " % time.ticks_diff(time.ticks_ms(), T0) + " ".join(str(x) for x in a)
    print(line)
    LOG.write(line + "\n")
    LOG.flush()


dev = usbif.auto.device()
dev.functions("cdc", "hid")
log("costume cdc+hid; functions", dev.functions())
time.sleep_ms(1500)
display_drv.fill(0x0010)
display_drv.fill_rect(200, 200, 320, 320, 0x07E0)
try:
    display_drv.show()
except Exception:
    pass


def hid(kind, report, retries=50):
    for _ in range(retries):
        if dev.hid_send(kind, report):
            return True
        time.sleep_ms(2)
    return False


received = []


def on_input(kind, data):
    received.append((kind, data))


class Driver:
    """Sends the laptop real input from the P4, one step per cast frame, once
    the receiver has attached the back channel."""

    def __init__(self, session):
        self.s = session
        self.n = -1

    def step(self):
        if self.s.uibc_packets < 2:
            return
        self.n += 1
        n = self.n
        if n == 0:
            log("driver: back channel is up (%d packets); moving the cursor" % self.s.uibc_packets)
        if 5 <= n < 25:
            hid(dev.HID_MOUSE, bytes([0, 6, 0, 0]))          # +6 px right
        elif n == 30:
            log("driver: click"); hid(dev.HID_MOUSE, bytes([1, 0, 0, 0]))
        elif n == 33:
            hid(dev.HID_MOUSE, bytes([0, 0, 0, 0]))
        elif 40 <= n < 60:
            hid(dev.HID_MOUSE, bytes([0, 0xFA, 0, 0]))       # -6 px back
        elif n == 70:
            log("driver: key a"); hid(dev.HID_KEYBOARD, bytes([0, 0, 4, 0, 0, 0, 0, 0]))
        elif n == 74:
            hid(dev.HID_KEYBOARD, bytes([0, 0, 0, 0, 0, 0, 0, 0]))
        elif n == 80:
            log("driver: done; %d UIBC packets so far" % self.s.uibc_packets)


gc.collect()
s = Session(SINK, log=log, on_input=on_input)
drv = Driver(s)


def make(dst_ip, dst_port, server_port):
    return LiveStreamer(dst_ip, dst_port, server_port, fb, fps=30, bitrate=3_000_000, scene=drv, seconds=SECONDS, log=log)


try:
    result = s.run(make, seconds=SECONDS + 15, idle_after_done=2)
    log("result:", result, "received", len(received))
    for k, d in received[:40]:
        log("  ", k, d.hex() if isinstance(d, (bytes, bytearray)) else d)
except Exception as e:
    log("EXC", repr(e))
LOG.close()
print("UIBC_TEST_DONE")
