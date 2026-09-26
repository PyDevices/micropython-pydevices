# The T-Embed (ESP32-S3) comparison: the same kind of MJPEG clip at the panel's
# 320x170, decoded in software by jpegio (TJpgDec) and blitted band by band.
import time, struct, gc
import jpegio
from board_config import display_drv

PATH = "/test170.mjpeg"
FPS = 30
LOOPS = 3
LOG = open("/play_mjpeg.log", "w")


def log(*a):
    line = " ".join(str(x) for x in a)
    print(line)
    LOG.write(line + "\n")
    LOG.flush()


idx = open(PATH + ".idx", "rb").read()
sizes = [struct.unpack_from("<I", idx, 4 * i)[0] for i in range(len(idx) // 4)]
f = open(PATH, "rb")
buf = bytearray(max(sizes))
mv = memoryview(buf)
dec = jpegio.JpegDecoder()
blit = display_drv.blit_rect
gc.collect()
log("frames %d, largest %d bytes, panel %dx%d, free %d" % (len(sizes), max(sizes), display_drv.width, display_drv.height, gc.mem_free()))
n = dropped = 0
dec_us = dec_max = 0
t0 = time.ticks_us()
total = len(sizes) * LOOPS
while n < total:
    k = n % len(sizes)
    if k == 0 and n:
        f.seek(0)
    now = time.ticks_diff(time.ticks_us(), t0)
    due = n * 1000000 // FPS
    if now < due:
        time.sleep_us(due - now)
    elif now - due > 1000000 // FPS and n:
        f.seek(sizes[k], 1)
        dropped += 1
        n += 1
        continue
    f.readinto(mv[:sizes[k]])
    b = time.ticks_us()
    w, h = dec.open(mv[:sizes[k]])
    dec.decode(lambda x, y, ww, hh, m: blit(m, x, y, ww, hh), 1)
    d = time.ticks_diff(time.ticks_us(), b)
    dec_us += d
    if d > dec_max:
        dec_max = d
    n += 1
el = time.ticks_diff(time.ticks_us(), t0)
log("done: %d frames in %.1f s = %.1f fps shown, decode+blit avg %d us max %d, dropped %d, free %d" % (
    n, el / 1e6, (n - dropped) * 1e6 / el, dec_us // max(1, n - dropped), dec_max, dropped, gc.mem_free()))
LOG.close()
print("PLAY_DONE")
