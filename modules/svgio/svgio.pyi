# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""SVG decoding.

`SvgDecoder` reads an SVG and draws it at any size into an RGB565 buffer,
with an alpha byte for each pixel or blended over what the buffer already
holds. Icons and gauges stay sharp at every size from one file.

It is NanoSVG's parser and rasteriser: shapes, paths, strokes, transforms,
opacity, linear and radial gradients, and class rules in a `<style>` sheet.
Text, `<use>`, clip paths, masks, patterns, filters and embedded images are
left out, so convert text to paths before you export.

The same code runs on every MicroPython port. On CPython, the `svgio` module
in the desktop package gives the same API over resvg.
"""

__revision__: str
"""Which source this firmware was built from, from `git describe`: a release
tag, or the nearest one and the commit. "unknown" when the build had no git."""

class SvgDecoder:
    """Draws SVGs into RGB565 buffers.

    The SVG stays parsed after `open()`, so `decode()` can draw it again,
    into another buffer or at another place, without reading it again.
    """

    def __init__(self) -> None:
        """Make a decoder."""
    def open(
        self,
        source: str | bytes | bytearray | memoryview,
        *,
        width: int | None = None,
        height: int | None = None,
        scale: float | None = None,
    ) -> tuple[int, int]:
        """Read and parse an SVG, and choose the size to draw it.

        Without `width`, `height` or `scale` the size is the SVG's own, from
        its `width` and `height` or its `viewBox`. With one of them the other
        side follows the SVG's aspect ratio, rounded to a whole pixel.

        Args:
            source: A path, the SVG's bytes, or a binary stream.
            width: Draw it this many pixels wide.
            height: Draw it this many pixels high.
            scale: Draw it this many times its own size.

        Returns:
            The size `decode()` will draw, as `(width, height)`.

        Raises:
            ValueError: More than one of `width`, `height` and `scale`, a size
                outside 1 to 32767 pixels a side, or data that isn't an SVG
                with a size.
            MemoryError: The parsed SVG doesn't fit in the heap.
        """
    def decode(
        self,
        target: bytearray | memoryview | None,
        alpha: bytearray | memoryview | None = None,
        *,
        x: int = 0,
        y: int = 0,
        stride: int | None = None,
    ) -> None:
        """Draw the SVG.

        With an `alpha` buffer, `target` gets the colour and `alpha` the
        coverage, 0 to 255, so you can blend it later, or hand both to LVGL as
        RGB565A8. With `target` alone it is blended over what `target` already
        holds, so an icon lands on any background. With `alpha` alone, and
        `target` None, you get just the shape, a mask for
        `FrameBuffer.blit_alpha` to fill with one colour.

        Pixels outside the image are left alone. The drawing runs in strips
        of up to 64 KB, so a full-screen picture needs no full-screen RGBA
        buffer.

        Args:
            target: Native-order RGB565, two bytes a pixel, or None.
            alpha: One byte a pixel, with the same `x`, `y` and `stride`.
            x: Column of the image's left edge in the buffers.
            y: Row of the image's top edge in the buffers.
            stride: Pixels from the start of one row to the next. The default
                is the image's width.

        Raises:
            RuntimeError: No `open()` yet.
            ValueError: Neither buffer, or a buffer too small for the image at
                that place (the message gives the sizes).
            MemoryError: No room for a strip.
        """
    @property
    def width(self) -> int:
        """The width `decode()` draws, after `open()`."""
    @property
    def height(self) -> int:
        """The height `decode()` draws, after `open()`."""

def register_lvgl_decoder() -> bool:
    """Add this module's decoder to the running LVGL.

    After this, `lv.image` shows SVGs, from a file whose name ends in `.svg`
    or from a variable holding the SVG's text, at the SVG's own size. It
    registers itself on `import svgio` when LVGL is already initialised, so
    call it when `import svgio` came before `lv.init()`, and after every
    `lv.deinit()` and `lv.init()` pair.

    Returns:
        True when this call added the decoder, False when it was already
        there.

    Raises:
        RuntimeError: LVGL is not initialised, or the firmware was built
            without the `lvgl` module (the message says which).
    """
def lvgl_decoders() -> tuple[str, ...]:
    """List LVGL's image decoders by name, in the order LVGL tries them.

    Returns:
        For example `("svgio", "jpegio", "LODEPNG", "BIN")` once this module's
        decoder is registered, and the empty tuple before `lv.init()` or in a
        firmware built without `lvgl`.
    """
