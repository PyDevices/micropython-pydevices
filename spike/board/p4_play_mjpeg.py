# Play an MJPEG file (raw concatenated JPEGs plus a .idx of frame sizes) on the
# panel through the P4's hardware JPEG decoder, straight into the framebuffer,
# paced by the clock; print decode ms per frame, fps and drops.
import sys
sys.path.insert(0, "/cast")
import time, struct, gc
import jpegdec
from board_config import fb, display_drv

PATH = "/cast/test720.mjpeg"
FPS = 30
LOOPS = 3
FROM_RAM = True   # preload the clip: the flash read (40 ms a frame) is the bottleneck, not the decoder
LOG = open("/cast/play_mjpeg.log", "w")


def log(*a):
    line = " ".join(str(x) for x in a)
    print(line)
    LOG.write(line + "\n")
    LOG.flush()


idx = open(PATH + ".idx", "rb").read()
sizes = [struct.unpack_from("<I", idx, 4 * i)[0] for i in range(len(idx) // 4)]
f = open(PATH, "rb")
log("loading %s%s" % (PATH, " into RAM" if FROM_RAM else ""))
if FROM_RAM:
    clip = memoryview(f.read())
    offsets = []
    o = 0
    for sz in sizes:
        offsets.append(o)
        o += sz
buf = bytearray(max(sizes))
mv = memoryview(buf)
dec = jpegdec.Decoder()
info = None
gc.collect()
log("frames %d, largest %d bytes, free %d" % (len(sizes), max(sizes), gc.mem_free()))
n = 0
dropped = 0
dec_us = 0
dec_max = 0
read_us = 0
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
        # a whole frame late: drop this one (skip its bytes)
        if not FROM_RAM:
            f.seek(sizes[k], 1)
        dropped += 1
        n += 1
        continue
    a = time.ticks_us()
    if FROM_RAM:
        frame = clip[offsets[k]:offsets[k] + sizes[k]]
    else:
        f.readinto(mv[:sizes[k]])
        frame = mv[:sizes[k]]
    b = time.ticks_us()
    dec.decode(frame, fb)
    c = time.ticks_us()
    if info is None:
        info = dec.info
        log("first frame: %s, decode %d us" % (info, time.ticks_diff(c, b)))
    d = time.ticks_diff(c, b)
    dec_us += d
    if d > dec_max:
        dec_max = d
    read_us += time.ticks_diff(b, a)
    n += 1
    if n % 150 == 0:
        el = time.ticks_diff(time.ticks_us(), t0)
        log("frame %d: %.1f fps, decode avg %d us max %d, read avg %d us, dropped %d" % (n, (n - dropped) * 1e6 / el, dec_us // (n - dropped), dec_max, read_us // (n - dropped), dropped))
el = time.ticks_diff(time.ticks_us(), t0)
log("done: %d frames in %.1f s = %.1f fps shown, decode avg %d us max %d, read avg %d us, dropped %d, free %d" % (
    n, el / 1e6, (n - dropped) * 1e6 / el, dec_us // max(1, n - dropped), dec_max, read_us // max(1, n - dropped), dropped, gc.mem_free()))
dec.close()
LOG.close()
print("PLAY_DONE")
