# svgio — SVG decode

`import svgio` draws an SVG at any size into an RGB565 buffer. One icon file
gives you a sharp 24 px status icon and a 96 px button, and one gauge file
fills any screen.

```python
import svgio
d = svgio.SvgDecoder()
w, h = d.open("wifi.svg", width=48)                 # or bytes, or a stream

pixels, alpha = bytearray(w * h * 2), bytearray(w * h)
d.decode(pixels, alpha)                             # colour and coverage

d.decode(fb_buf, x=10, y=10, stride=480)            # blended over what fb_buf holds

mask = bytearray(w * h)
d.decode(None, mask)                                # the shape alone
fb.blit_alpha(mask, 10, 10, w, h, 0xFFFF)           # filled with one colour
```

The same code runs on every runtime. On MicroPython it's this C module, on
every port (`--modules all` has it). On CPython, `svgio.py` in
pydevices-desktop gives the same API over resvg, so `pip install resvg-py
pillow` is all a desktop needs.

[`examples/svgio_simpletest.py`](examples/svgio_simpletest.py) draws an icon at two sizes and prints it as text.

## What it draws

Shapes, paths, strokes with their joins, caps and dashes, transforms,
opacity, linear and radial gradients, and class rules in a `<style>` sheet.
That covers the icons and gauges most design tools export.

It leaves out text, `<use>`, clip paths, masks, patterns, filters and
embedded images. Convert text to paths before you export, and expand `<use>`
(Inkscape's "Unlink clone", or `svgo` with its default plugins).

## API

### `svgio.SvgDecoder()`

### `open(source, *, width=None, height=None, scale=None) -> (width, height)`

`source` is bytes-like, a path or a binary stream. Without a size, you get
the SVG's own, from its `width` and `height` or its `viewBox`. Give one of
`width`, `height` or `scale` and the other side follows the aspect ratio.
`width` and `height` are properties too, after a successful `open()`.

The SVG stays parsed, so you can `decode()` it as often as you like.

### `decode(target, alpha=None, *, x=0, y=0, stride=None)`

Draws the image with its top-left at `(x, y)` and rows `stride` pixels apart
(default: the image's width). Pixels outside the image are left alone.

- `target` and `alpha`: native-order RGB565 colour into `target`, and
  coverage, 0 to 255, into `alpha`. That pair is LVGL's RGB565A8.
- `target` alone: blended over what `target` holds, so an icon lands cleanly
  on any background.
- `alpha` alone, with `target` None: the shape as a mask, for
  `FrameBuffer.blit_alpha` to fill with a colour.

Bad arguments and data that isn't an SVG raise `ValueError`, saying what's
wrong. A parse or a strip that doesn't fit in the heap raises `MemoryError`.

## Memory and speed

`open()` holds the SVG's text while it parses, then the parsed shapes. A
48 px icon parses to a few KB. `decode()` draws in strips of up to 64 KB of
RGBA, so an 800x480 picture never needs a full-screen buffer beyond your
own.

On a desktop, an 800x480 gauge with gradients draws in a few milliseconds,
and an icon in well under one.

## LVGL image decoder

Beside the `lvgl` usermod, svgio is also an LVGL image decoder named
`"svgio"`, so `lv.image` shows SVGs:

```python
import lvgl as lv
import svgio
lv.init()
svgio.register_lvgl_decoder()      # needed only if `import svgio` ran before lv.init()

img = lv.image(lv.screen_active())
img.set_src("S:/icons/wifi.svg")   # a file, through a registered lv.fs_drv_t

with open("wifi.svg", "rb") as f:  # or the SVG's text in a variable
    svg = f.read()
dsc = lv.image_dsc_t({"header": {"cf": lv.COLOR_FORMAT.RAW}, "data_size": len(svg), "data": svg})
img.set_src(dsc)
```

It claims a file whose name ends in `.svg`, and a variable whose text starts
with `<?xml` or `<svg`. It draws at the SVG's own size, as LVGL's own SVG
support does, into an RGB565A8 draw buffer, so edges blend over what's
behind. Files over 512 KB are refused. A firmware with this decoder doesn't
need LVGL's `LV_USE_SVG` and ThorVG.

Registration works as in jpegio: `import svgio` after `lv.init()` registers
it, and `register_lvgl_decoder()` does it by hand, after every `lv.deinit()`
and `lv.init()` pair too. `lvgl_decoders()` lists LVGL's decoders in the
order it asks them. Both names exist on every build; on one without `lvgl`
the first raises `RuntimeError` and the second returns `()`. Why they are
unconditional is in [jpegio's README](../jpegio/README.md#lvgl-image-decoder).

## How

[NanoSVG](https://github.com/memononen/nanosvg) parses the SVG into cubic
Bézier paths and rasterises them with anti-aliasing, a strip at a time, into
RGBA. svgio turns each strip into RGB565 and alpha, or blends it. NanoSVG's
memory comes from the MicroPython heap, so running out raises `MemoryError`
instead of failing in a C library, and its few C library calls go to small
local versions, so every port links it. The vendored copy, its commit and
one fix to it are in [`third_party/nanosvg/ORIGIN.txt`](third_party/nanosvg/ORIGIN.txt); its licence is
in [`NOTICE`](NOTICE).

## Tests

[`tests/test_svgio.py`](tests/test_svgio.py) runs on any MicroPython and on CPython: sizes, sources,
colour and alpha, placement, blending and the errors, then every SVG in
[`tests/svg/`](tests/svg/) timed. [`tests/check_svgio_references.py`](tests/check_svgio_references.py) compares those renders on the
PC with PNGs that resvg drew, which
[`tests/make_references.py`](tests/make_references.py) made.

```bash
builds/unix/standard/micropython -X heapsize=32M modules/svgio/tests/test_svgio.py --out /tmp/svg
python modules/svgio/tests/check_svgio_references.py /tmp/svg --sheet sheet.png
```
