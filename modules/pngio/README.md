# pngio — PNG encode and decode

`import pngio` turns an RGB565 framebuffer into a PNG and back. It compresses
for real: a 480x270 screen of flat panels encodes to about 3 KB, not the
389 KB of an uncompressed one, in a couple of milliseconds on a desktop.

```python
import pngio
png = pngio.PngEncoder().encode(fb, 480, 270)       # bytes

d = pngio.PngDecoder()
w, h = d.open("logo.png")                           # or bytes, or a stream
d.decode(fb, 10, 10, stride=480)                    # RGB565 into fb at (10, 10)
```

The same code runs on every runtime. On MicroPython it's this C module, on
every port (`--modules all` has it). On CPython, `pngio.py` in
pydevices-desktop gives the same API over Pillow, so `pip install pillow` is
all a desktop needs.

## API

### `pngio.PngEncoder(level=1)`

`level` is 1 (fastest) to 9 (smallest). Keep one encoder and reuse it: its
scratch memory stays allocated between frames.

### `encode(buffer, width, height, *, format=pngio.RGB565, stride=None, swap=False) -> bytes`

`format` is `RGB565` (native-order 16-bit, what displaydev framebuffers hold),
`GS8` (a byte of grey) or `RGB888` (R, G, B bytes). `stride` is the distance
between rows in pixels, for encoding a block of a bigger picture. `swap=True`
reads RGB565 as big-endian. RGB565 becomes 8-bit RGB by bit replication, so
31 is 255.

### `pngio.PngDecoder()`

### `open(source) -> (width, height)`

`source` is bytes-like, a path or a binary stream. `width` and `height` are
properties too, after a successful `open()`.

### `decode(target, x=0, y=0, *, stride=None)`

Native-order RGB565 into a writable buffer, with the image's top-left at
`(x, y)` and rows `stride` pixels apart (default: the image's width). Pixels
outside the image are left alone. It reads 8-bit grey, RGB, palette, grey with
alpha and RGBA, and 1, 2 and 4-bit grey and palette; alpha is dropped. It
refuses interlaced PNGs and 16-bit depth. One decode per `open()`.

Bad arguments and bad data raise `ValueError`, saying what's wrong. A corrupt
image is caught by its checksum. On a firmware without the `deflate` module,
`decode()` raises `OSError`, because that's the inflater it uses.

## How

Each row takes the PNG filter (None, Sub, Up or Paeth) whose output has the
smallest sum, which turns a flat screen into runs of zeros. The image is then
one deflate stream with fixed Huffman codes. Matches come from a hash of the
next three bytes, with a chain of earlier positions that the level makes
deeper. Decoding inflates one scanline at a time through MicroPython's own
uzlib, so it holds two rows and a 32 KB window rather than the whole image.

## Tests

`tests/test_pngio.py` runs on any MicroPython and on CPython: exact round
trips, the formats, a stride and a placed decode, a UI frame's size, and the
errors. `tests/check_pngio_pillow.py` checks both ways on the PC. Pillow
reads every file pngio wrote, and pngio reads Pillow's files in every layout
it supports, with Pillow's own filters and dynamic Huffman blocks.

```bash
builds/unix/standard/micropython -X heapsize=32M modules/pngio/tests/test_pngio.py --out /tmp/png
python modules/pngio/tests/check_pngio_pillow.py builds/unix/standard/micropython /tmp/png
```
