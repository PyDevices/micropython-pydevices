# offheap — big buffers the garbage collector skips

`import offheap` gives you byte buffers that live outside MicroPython's
garbage-collected heap. Use one wherever you'd make a big `bytearray` for
pixels: a frame, a sprite sheet, an off-screen background. The collector reads
every word of every `bytearray` a program holds, in case one is a pointer, so a
few megabytes of pixels make each collection stop the program for a tenth of a
second or more. It doesn't look inside a `Buffer`.

```python
import offheap
from pygraphics import RGB565, FrameBuffer

pixels = offheap.Buffer(800 * 480 * 2)      # zeroed, like bytearray(n)
frame = FrameBuffer(pixels, 800, 480, RGB565)
frame.fill(0x001F)
```

A `Buffer` has the buffer protocol, so pygraphics' `FrameBuffer`, `framebuf`,
`memoryview` and the display drivers take it as they take a `bytearray`.

On an ESP32-S31 with an 800x480 panel, a game holding three full-screen
backgrounds and a sprite sheet (2.4 MB) paused for 240 to 290 ms at each
collection with them in bytearrays, and for 47 to 71 ms with them in Buffers.
`gc.collect()` alone went from about 195 ms to 25 ms.

| Port | Where the bytes come from |
|---|---|
| esp32 | PSRAM through `heap_caps`, 64-byte aligned (internal RAM on a board without PSRAM) |
| unix, windows, webassembly | C's `malloc` |

`--modules all` includes it. The other ports (rp2, stm32, mimxrt, samd, nrf)
give all their RAM to the garbage collector and have no allocator beside it,
so they have no `offheap`. A program that runs everywhere falls back on its own:

```python
try:
    from offheap import Buffer as pixel_bytes
except ImportError:
    pixel_bytes = bytearray
```

[`examples/offheap_simpletest.py`](examples/offheap_simpletest.py) times a
collection with a frame in a `bytearray` and in a `Buffer`.

## API

### `offheap.Buffer(size)`

`size` zeroed bytes outside the heap. `len()` is the size. Index or slice it
through `memoryview(buffer)`. A negative size raises `ValueError`. When there
isn't room, it collects once, so that Buffers nobody holds give their bytes
back, and tries again before it raises `MemoryError`.

### `deinit()`

Frees the bytes now. Calling it again does nothing; using the `Buffer`
afterwards (`len()`, `memoryview()`, a new `FrameBuffer`) raises `ValueError`.
`with offheap.Buffer(n) as b:` calls it at the end of the block.

### `offheap.mem_alloc()`

How many bytes live Buffers hold, the counterpart of `gc.mem_alloc()`, which
doesn't count them.

## Keep the Buffer

The bytes are freed by `deinit()`, when the `Buffer` is collected, or at a soft
reset, whichever comes first. A `memoryview` of a `Buffer`, or a driver that
took its address, doesn't keep the `Buffer` alive: the collector can't see a
reference into memory it doesn't manage. So keep the `Buffer` itself for as
long as anything uses its bytes.

```python
view = memoryview(offheap.Buffer(4096))    # wrong: nothing holds the Buffer
pixels = offheap.Buffer(4096)              # right: keep pixels beside the view
view = memoryview(pixels)
```

`framebuf.FrameBuffer` and pygraphics' `FrameBuffer` hold the object they were
given, so a frame made from a `Buffer` keeps it alive.

A soft reset (Ctrl-D, or a launcher restarting into the next app) frees every
`Buffer`'s bytes, so a program that never calls `deinit()` leaks nothing into
the next one.

## Build

It's a C module of this repository's firmware build:

```bash
python3 build_mp.py --port unix --modules offheap
```

Then run the tests with the interpreter it built:

```bash
./builds/unix/standard/micropython modules/offheap/tests/test_offheap.py
```
