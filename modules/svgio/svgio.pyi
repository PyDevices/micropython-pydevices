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

    def __init__(self, *, memory: int | None = None) -> None:
        """Make a decoder.

        With `memory`, the decoder takes that many bytes once and does every
        `open()` and `decode()` inside them, so drawing asks the heap for
        nothing. They come from outside MicroPython's heap where the port has
        memory there, so the collector never reads them. A picture that needs
        more still draws.

        Args:
            memory: Bytes of working memory, at least 4096, or None.

        Raises:
            ValueError: `memory` under 4096.
            MemoryError: The heap has no block that large.
        """
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
            source: A path, the SVG's bytes, or a binary stream. The bytes
                may be a drawing in the compact form instead of SVG text.
            width: Draw it this many pixels wide.
            height: Draw it this many pixels high.
            scale: Draw it this many times its own size.

        Returns:
            The size `decode()` will draw, as `(width, height)`.

        Raises:
            ValueError: More than one of `width`, `height` and `scale`, a size
                outside 1 to 32767 pixels a side, data that isn't an SVG
                with a size, or a compact drawing whose records are cut short
                or were made for another size of float.
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
        rows: tuple[int, int] | None = None,
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
            rows: Draw only the image's rows from `rows[0]` up to, not
                including, `rows[1]`, where they sit in the whole picture.
                Bands drawn one after another give the same bytes as one
                `decode()` of every row. The default is every row.

        Raises:
            RuntimeError: No `open()` yet.
            ValueError: Neither buffer, a buffer too small for the rows drawn
                at that place (the message gives the sizes), or `rows` outside
                the image.
            MemoryError: No room for a strip.
        """
    def close(self) -> None:
        """Let go of the SVG and of the working memory now.

        Collecting the decoder does the same.
        """
    @property
    def width(self) -> int:
        """The width `decode()` draws, after `open()`."""
    @property
    def height(self) -> int:
        """The height `decode()` draws, after `open()`."""
    @property
    def memory(self) -> int:
        """The bytes of working memory the decoder was made with, or 0."""
    @property
    def memory_peak(self) -> int:
        """The most bytes the decoder held at once since the last `open()`.

        Larger than `memory` when a picture didn't fit and the rest came from
        the heap.
        """

class Buffer:
    """Bytes to draw pictures into, outside MicroPython's heap where the port has memory there.

    The collector reads every block of the heap it keeps, so megabytes of
    pictures in `bytearray`s make each collection slow. It doesn't read a
    `Buffer`. Pass one to `SvgDecoder.decode()`, or a `memoryview` of part of
    one; it isn't cleared.

    A `memoryview` of a `Buffer` stays safe to use after the `Buffer` is
    collected: its bytes aren't given back to the system then, but kept for
    the next `Buffer` they fit. Only `close()` returns them, so call it when
    you are done.
    """

    def __init__(self, size: int) -> None:
        """Take `size` bytes.

        Raises:
            ValueError: `size` under 1.
            MemoryError: No block that large.
        """
    def __len__(self) -> int: ...
    def __buffer__(self, flags: int, /) -> memoryview: ...
    def close(self) -> None:
        """Give the bytes back to the system now. A `memoryview` of them must not be used again."""

def expand(drawing: bytes | bytearray | memoryview) -> bytes:
    """Give the SVG text a drawing in the compact form stands for.

    The compact form is SVG with its numbers left as numbers: records that
    `struct.pack` makes, which `SvgDecoder.open()` takes where it takes text.
    This is the text `open()` parses for one, each number written as
    ``"%.5g"`` prints it. The module's README lists the records.

    Args:
        drawing: The records, starting ``b"\\x01SVG"`` and the size of a float.

    Returns:
        The SVG text, as bytes.

    Raises:
        ValueError: Not a compact drawing, one cut short, or one made for
            another size of float.
        MemoryError: No room for the text.
    """

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
