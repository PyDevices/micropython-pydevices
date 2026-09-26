import sys
sys.path.insert(0, "/cast")
import house_panel
LOG = open("/cast/house.log", "w")
def log(*a):
    s = " ".join(str(x) for x in a); print(s); LOG.write(s + "\n"); LOG.flush()
house_panel.run(seconds=18, auto_cast_after=0, log=log)
from board_config import display_drv
b = memoryview(display_drv._buffer); out = bytearray(180*180*2); k = 0
for y in range(0, 720, 4):
    base = y*1440
    for x in range(0, 1440, 8):
        out[k] = b[base+x]; out[k+1] = b[base+x+1]; k += 2
f = open("/cast/house_panel.rgb", "wb"); f.write(out); f.close()
LOG.close(); print("HOUSE_TEST_DONE")
