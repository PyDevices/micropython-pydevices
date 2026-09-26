# Bench the hardware H.264 encoder: the panel's own framebuffer at 720x720, and a
# synthetic 1280x720 RGB565 buffer. Prints us per frame and bytes per frame.
import sys
sys.path.insert(0, "/cast")
import time, gc
import h264enc
LOG = open("/cast/enc_bench.log", "w")


def log(*a):
    line = " ".join(str(x) for x in a)
    print(line)
    LOG.write(line + "\n")
    LOG.flush()


def bench(name, enc, src, frames=90):
    out = bytearray(enc.width * enc.height)
    us = []
    sizes = []
    types = {}
    for i in range(frames):
        n = enc.encode(src, out)
        us.append(enc.us)
        sizes.append(n)
        types[enc.frame_type] = types.get(enc.frame_type, 0) + 1
    us.sort()
    log("%s: %d frames, encode us min %d med %d max %d (last ppa %d us), bytes min %d avg %d max %d, types %s" % (
        name, frames, us[0], us[len(us) // 2], us[-1], enc.ppa_us, min(sizes), sum(sizes) // len(sizes), max(sizes), types))
    return out


log("h264enc available:", h264enc.available())
gc.collect()
log("mem free before:", gc.mem_free())
try:
    from board_config import fb, display_drv
    log("panel framebuffer:", fb.width, "x", fb.height, "stride", fb.row_stride, "bytes", len(memoryview(fb)))
    enc = h264enc.Encoder(fb.width, fb.height, fps=30, gop=30, bitrate=3_000_000)
    # a solid red panel: in packed YUV420 the first line should read U Y Y U Y Y (90 82 82 ...)
    # and the second line V Y Y (240 82 82 ...) for BT.601 limited range
    display_drv.fill(0xF800)
    try:
        display_drv.show()
    except Exception:
        pass
    n = enc.encode(fb, None)
    log("first frame: %d bytes type %d in %d us (ppa %d us)" % (n, enc.frame_type, enc.us, enc.ppa_us))
    yuv = enc.yuv()
    log("yuv line0[0:12]:", yuv[0:12].hex(), " line1[0:12]:", yuv[1080:1092].hex(), " len", len(yuv))
    first = enc.last()
    log("first bytes:", first[:16].hex())
    bench("panel 720x720 3 Mbps", enc, fb)
    enc.set_bitrate(6_000_000)
    bench("panel 720x720 6 Mbps", enc, fb)
    enc.close()
except Exception as e:
    log("panel bench failed:", repr(e))
gc.collect()
try:
    buf = bytearray(1280 * 720 * 2)
    for i in range(0, len(buf), 2):
        buf[i] = i & 0xFF
        buf[i + 1] = (i >> 9) & 0xFF
        if i > 400000:
            break
    enc = h264enc.Encoder(1280, 720, fps=30, gop=30, bitrate=4_000_000)
    bench("synthetic 1280x720 4 Mbps", enc, buf, frames=60)
    enc.close()
except Exception as e:
    log("720p bench failed:", repr(e))
log("mem free after:", gc.mem_free())
LOG.close()
print("BENCH_DONE")
