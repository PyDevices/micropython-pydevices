import sys, time
sys.path.insert(0, "/lib/examples/roku_remote")
from examples.roku_remote.roku_remote import main
main()
time.sleep(8)
import time as _t; _t0=_t.ticks_ms()
from board_config import display_drv
import display_driver
_e = display_driver.event_loop.current_instance()
_e.disable()
from array import array
b = memoryview(display_drv._buffer)
out = bytearray(180 * 180 * 2)
k = 0
for y in range(0, 720, 4):
    base = y * 1440
    for x in range(0, 1440, 8):
        out[k] = b[base + x]; out[k + 1] = b[base + x + 1]; k += 2
with open("/cast/roku_panel.rgb", "wb") as f:
    f.write(out)
a = out
print("SNAP", len(a), _t.ticks_diff(_t.ticks_ms(), _t0), "ms")
