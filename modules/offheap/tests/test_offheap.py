# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

# offheap on a desktop MicroPython (unix, windows or webassembly) or a board:
# the API, its errors, freeing on deinit and on collection, drawing into a
# Buffer, and how long a collection takes with big buffers alive.
#
#   micropython modules/offheap/tests/test_offheap.py
#
# Prints PASS or FAIL per check and ends with a count; exits 1 on a failure.

import gc
import sys
import time

import offheap

failures = 0


def check(name, ok, detail=""):
    global failures
    print(("PASS " if ok else "FAIL ") + name + (": " + str(detail) if detail and not ok else ""))
    if not ok:
        failures += 1


def raises(exc, fn, *args):
    try:
        fn(*args)
    except exc as e:
        return str(e) or exc.__name__
    return None


# The fastest of three: the first collection after garbage also sweeps it.
def collect_ms():
    best = None
    for _ in range(3):
        t = time.ticks_us()
        gc.collect()
        ms = time.ticks_diff(time.ticks_us(), t) / 1000
        best = ms if best is None or ms < best else best
    return best


gc.collect()
base = offheap.mem_alloc()

# A Buffer is zeroed bytes with a length, like bytearray(n).
buf = offheap.Buffer(4096)
check("len", len(buf) == 4096, len(buf))
check("mem_alloc counts it", offheap.mem_alloc() - base == 4096, offheap.mem_alloc() - base)
view = memoryview(buf)
check("zeroed", bytes(view[:16]) == bytes(16) and view[4095] == 0)
check("repr", repr(buf) == "<Buffer 4096 bytes>", repr(buf))
check("revision is a string", isinstance(offheap.__revision__, str) and len(offheap.__revision__) > 0)

# Writes through one view are seen through another, and by bytes().
view[0:4] = b"\x01\x02\x03\x04"
view[-1] = 0xAB
check("write and read back", bytes(memoryview(buf)[:4]) == b"\x01\x02\x03\x04" and bytes(buf)[-1] == 0xAB)
check("bytearray copy", bytearray(buf)[:4] == bytearray(b"\x01\x02\x03\x04"))
check("truth", bool(buf) and not bool(offheap.Buffer(0)))

# A drawing library takes it through the buffer protocol.
try:
    import framebuf
except ImportError:
    framebuf = None
if framebuf is not None:
    fb = framebuf.FrameBuffer(buf, 32, 64, framebuf.RGB565)
    fb.fill(0)
    fb.pixel(3, 2, 0xF800)
    check("framebuf draws into it", fb.pixel(3, 2) == 0xF800 and view[(2 * 32 + 3) * 2 + 1] == 0xF8)
try:
    from pygraphics import RGB565, FrameBuffer
except ImportError:
    FrameBuffer = None
if FrameBuffer is not None:
    pfb = FrameBuffer(buf, 32, 64, RGB565)
    pfb.fill_rect(0, 0, 32, 64, 0x07E0)
    pfb.blit(FrameBuffer(bytearray(8), 2, 2, RGB565), 0, 0)
    check("pygraphics draws into it", pfb.pixel(5, 5) == 0x07E0 and pfb.pixel(0, 0) == 0)
else:
    print("SKIP pygraphics: not in this firmware")

# deinit gives the bytes back at once, and is safe twice.
buf.deinit()
buf.deinit()
check("deinit frees", offheap.mem_alloc() == base, offheap.mem_alloc() - base)
check("len after deinit raises", raises(ValueError, len, buf) is not None)
check("memoryview after deinit raises", raises(ValueError, memoryview, buf) is not None)
check("repr after deinit", repr(buf) == "<Buffer deinitialised>", repr(buf))

# A with block deinits at its end.
with offheap.Buffer(100) as b:
    check("with gives the Buffer", len(b) == 100)
check("with frees", offheap.mem_alloc() == base and raises(ValueError, len, b) is not None)

# A Buffer nobody holds is freed when it's collected.
offheap.Buffer(10000)
gc.collect()
check("collected Buffer frees", offheap.mem_alloc() == base, offheap.mem_alloc() - base)

check("negative size", raises(ValueError, offheap.Buffer, -1) is not None)
check("not an int", raises(TypeError, offheap.Buffer, "x") is not None)
check("too big", raises(MemoryError, offheap.Buffer, (1 << (8 * 8 - 2)) - 1 if sys.maxsize > 1 << 32 else 0x7FFFFFFF) is not None)
check("nothing left", offheap.mem_alloc() == base, offheap.mem_alloc() - base)

# The point: bytes in a Buffer aren't scanned by the collector. Three 800x480
# RGB565 frames each way. Every word of the bytearrays is read in case it is a
# pointer; the Buffers are a few words each. Filled with a word that looks like
# a heap address, the bytearrays also make the collector chase it.
FRAME = 800 * 480 * 2
probe = bytearray(64)
# id() is signed, and negative for an address in the top half (PSRAM on ESP32).
size = 4 if sys.maxsize < 1 << 32 else 8
word = (id(probe) & ((1 << (8 * size)) - 1)).to_bytes(size, "little")
array_ms = None
try:
    arrays = [bytearray(word * (FRAME // size)) for _ in range(3)]
    array_ms = collect_ms()
except MemoryError:
    print("SKIP bytearray timing: the heap is too small for three frames")
arrays = None
buffers = [offheap.Buffer(FRAME) for _ in range(3)]
for b in buffers:
    memoryview(b)[:] = word * (FRAME // size)
buffer_ms = collect_ms()
for b in buffers:
    b.deinit()
# The same heap with the Buffers gone: anything they cost a collection shows here.
freed_ms = collect_ms()
print("gc.collect(): %s ms with three bytearray frames, %.1f ms with three Buffers, %.1f ms with none"
      % ("-" if array_ms is None else "%.1f" % array_ms, buffer_ms, freed_ms))
check("Buffers barely slow a collection", buffer_ms < freed_ms + 5, "%.1f vs %.1f ms" % (buffer_ms, freed_ms))
check("all freed", offheap.mem_alloc() == base, offheap.mem_alloc() - base)

print("%d failure%s" % (failures, "" if failures == 1 else "s"))
if failures:
    sys.exit(1)
