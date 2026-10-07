# ppa — scale, rotate, convert and fill pixel buffers

`import ppa` reshapes pictures in memory. It can scale, rotate, mirror,
crop, change the pixel format, fill a block and blend two pictures. On the
ESP32-P4 the chip's Pixel Processing Accelerator does the work, 6 to 30 times
faster than the CPU. Everywhere else, desktop included, the same calls
run in portable C, so a program that uses `ppa` runs anywhere.

```python
import ppa
ppa.srm(cam, 320, 240, fb, 1280, 720)                   # scale to fill
ppa.srm(src, w, h, dst, h, w, rotate=90)                # a quarter turn
ppa.convert(rgb, w, h, yuy2, ppa.RGB565, ppa.YUY2)      # same size, new format
ppa.fill(fb, 1280, 720, 0xFF8000, x=10, y=10, w=100, h=50)
ppa.blend(bg, fg, out, w, h, 128)                       # fg over bg, half and half
```

At 1280x720 on the P4, a scale from 320x240 takes 11 ms on the PPA against
176 ms in software. A quarter turn takes 32 ms against 1050 ms, an RGB565 to
YUV 4:2:0 conversion 32 ms against 199 ms, and a fill 7 ms.

`--modules all` includes it; name it with `--modules ppa` otherwise. The
software path builds on every port; the PPA is used only on the ESP32-P4.

## API

Buffers are anything with the buffer protocol: `bytearray`, `memoryview`, a
displaydev framebuffer. Every function checks a buffer's length against the
picture it is said to hold and raises `ValueError` when it is short.

### `ppa.srm(src, src_w, src_h, dst, dst_w, dst_h, *, src_fmt=RGB565, dst_fmt=RGB565, src_rect=None, x=0, y=0, w=0, h=0, rotate=0, mirror_x=False, mirror_y=False, swap=False, limited=False, approx=False, hardware=None) -> bool`

Scale, rotate and mirror in one operation, converting the format on the way.
The whole source (or the `src_rect=(x, y, w, h)` block of it) is scaled to fill
the `x, y, w, h` block of the destination (`w=0` and `h=0` mean the whole
picture). Pixels outside that block are left alone, which is how you put a
picture in the middle of a bigger one.

`rotate` is 0, 90, 180 or 270 degrees, counter-clockwise, as the PPA turns.
Scaling happens first, then the rotation, then the mirror. With a quarter turn
the destination block is the turned shape: a 320x240 source turned 90 fills a
240x320 block. Scaling is nearest-neighbour.

`swap=True` reads RGB565 as big-endian, the order an SPI panel's bytes are
often in. `limited=True` makes YUV studio range (16 to 235), what a video
encoder expects. Without it, YUV is full range (0 to 255), what a JPEG holds.

Returns whether the PPA did it. `hardware=None` uses the PPA when it can,
`False` never does, and `True` insists, raising `OSError` with the reason when
it can't.

### `ppa.convert(src, w, h, dst, src_fmt, dst_fmt, *, swap=False, limited=False, hardware=None) -> bool`

`srm` at the same size, for a format change alone.

### `ppa.fill(dst, dst_w, dst_h, color, *, x=0, y=0, w=0, h=0, fmt=RGB565)`

Fills a block with one colour, given as `0xRRGGBB` (or `0xAARRGGBB` for
ARGB8888). Takes RGB565, RGB888 and ARGB8888. The P4 only: elsewhere it raises
`OSError`, and so does a `dst` that breaks the alignment rule below.

### `ppa.blend(bg, fg, dst, w, h, alpha)`

Draws one RGB565 picture over another at `alpha` from 0 (all `bg`) to 255 (all
`fg`), into `dst`. The P4 only, with `dst` aligned as below.

### `ppa.size(fmt, w, h) -> int`

The bytes a `w` x `h` picture takes in a format.

### `ppa.HARDWARE`

`True` when this chip has a PPA.

### Formats

`RGB565` is 16-bit and native byte order, what displaydev framebuffers hold.
`RGB888` is three bytes (B, G, R), `ARGB8888` four (B, G, R, A), `GRAY8` one.
`YUY2` (Y0 U Y1 V) and `UYVY` (U Y0 V Y1) are 4:2:2, two bytes a pixel, what
webcams and USB video send. `YUV420` is the P4's packed 4:2:0 (U Y Y on the
first line of each pair, V Y Y on the second), 12 bits a pixel, what its H.264
encoder reads. BT.601 throughout.

## When the PPA says no

The PPA has rules, and a call it can't do runs in software unless you asked
for `hardware=True`. The PPA's rules are these:

- It scales in steps of 1/16, so 320 to 640 (2) and 320 to 200 (10/16) are
  fine, but 320 to 213 isn't. `approx=True` lets the PPA take the nearest step
  instead and fill a little less of the block. cameraif's `capture_scaled`
  does this, since a camera frame a few pixels short is better than one ten
  times slower.
- The destination buffer must start on a 64-byte boundary, a cache line, and
  a `bytearray`'s data is only 16-byte aligned. Allocate 64 bytes extra and
  hand over a `memoryview` slice that starts on the boundary, as
  `tests/test_ppa.py`'s `aligned()` does. The PPA also writes whole cache
  lines, so the buffer must hold the picture rounded up to the next 64
  bytes. An unaligned or short buffer still works, but in software.
- On a P4 before chip revision 3 (the Waveshare 4" panel), the PPA has no
  YUY2, UYVY or GRAY8. Conversion from RGB565 to one of those still goes
  through the PPA for the scale, and then through software for the format,
  which is quick. Reading one of those formats is software only. Revision 3
  boards, like the P4 WIFI6 DEV-KIT, do YUY2 and UYVY on the PPA.
- GRAY8 is BT.601 luma on every chip, so the PPA scales and software makes the
  grey, even on revision 3. The PPA's own GRAY8 is close to the plain mean of
  R, G and B: pure blue comes out 82 where luma is 28.

## Software and hardware agree

The software path copies the PPA's arithmetic, so a program sees the same
picture either way. RGB565 widens by zero-filling (31 becomes 248, not 255),
colour conversion truncates rather than rounds, YUY2 and UYVY take U from the
left pixel of each pair and V from the right, and YUV 4:2:0 takes U from the
top-left pixel of each 2x2 block and V from the bottom-left. Rotations, mirrors
and integer scales are byte-identical between the two. YUV 4:2:0 is within 2
in any byte, and so is a revision 3 PPA's full-range YUY2 (its limited range
is identical).

## For C

`src/ppa_mod.h` is the same thing for native code: `ppa_mod_open()`,
`ppa_mod_srm()`, `ppa_mod_used_hw()`, `ppa_mod_close()`. Each caller opens its
own client. h264enc's colour conversion and cameraif's `capture_scaled` and
`capture_yuy2` use it. A module that may be built without ppa declares these
weak and checks `ppa_mod_open()` for `NULL`.

## Tests

`tests/test_ppa.py` runs anywhere. On unix it proves the software path against
a Python model of each operation. On a P4 it adds the PPA against the software
path, the PPA's refusals, and the timings above.

```bash
builds/unix/standard/micropython -X heapsize=16M modules/ppa/tests/test_ppa.py
```
