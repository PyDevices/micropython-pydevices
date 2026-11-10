# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""JPEG decoding and encoding for MicroPython.

`JpegDecoder` reads a baseline JPEG from a file, a buffer or a stream into
RGB565 pixels, either whole into a buffer or one block at a time to a
callback, so a camera's frames can go straight to a display without a frame
buffer. It has the API of CircuitPython's own `jpegio`, so one script decodes
on both. `JpegEncoder` turns an RGB565 or greyscale buffer into JPEG bytes.

Pixels are in the chip's native byte order, which is what a display driver's
`blit_rect` takes.

On an ESP32-P4 both classes can use the chip's JPEG engine. Beside the `lvgl`
module the decoder is also LVGL's JPEG decoder, so an LVGL image shows JPEGs;
see `register_lvgl_decoder`.
"""

from typing import Callable

__revision__: str
"""Which source this firmware was built from, from `git describe`: a release
tag, or the nearest one and the commit. "unknown" when the build had no git."""

RGB565: int
"""`JpegEncoder.encode` format: two bytes per pixel, in the chip's native byte
order. What `JpegDecoder.decode` writes."""
GRAY: int
"""`JpegEncoder.encode` format: one byte per pixel."""

class JpegDecoder:
    """Decodes baseline JPEGs.

    Create one decoder and reuse it: it holds a work area of about 3.5 KB
    that is allocated once. `open` a source, then `decode` it, and open again
    before the next decode. Progressive, lossless, arithmetic-coded and CMYK
    JPEGs, and 4:1:1 or 4:4:0 chroma subsampling, are refused with
    `ValueError`.

    `width` and `height` are properties: the size of the last image opened.
    Reading them before a successful `open` raises `RuntimeError`.
    """

    width: int
    """Width in pixels of the image the last `open` accepted."""
    height: int
    """Height in pixels of the image the last `open` accepted."""
    hardware: bool
    """Whether the last `decode` ran on the chip's JPEG engine."""

    def __init__(self, *, hardware: bool = False) -> None:
        """Make a decoder.

        Args:
            hardware: True decodes on the ESP32-P4's JPEG engine when it can:
                a whole image into a buffer at scale 0. Anything else (a
                callable target, a scale, a JPEG the engine refuses) uses the
                software decoder, as before. The engine's pixels differ
                slightly from the software decoder's. On a chip without the
                engine, True raises OSError.
        """
    def open(self, source: str | bytes | bytearray | memoryview | object, /) -> tuple[int, int]:
        """Read a JPEG's headers and return its size.

        Args:
            source: A path; a bytes-like object, which is read in place and
                must stay unchanged until `decode` returns; or a binary
                stream such as an open file, `io.BytesIO` or a socket.

        Returns:
            (width, height) in pixels.

        Raises:
            ValueError: The input ended early, or is not a baseline JPEG. A
                frame from a camera that leaves out the standard Huffman
                tables fails here.
            MemoryError: The image's tables do not fit the work area.
            TypeError: The source is not a path, a buffer or a binary stream.
        """
    def decode(
        self,
        target: bytearray | memoryview | Callable[[int, int, int, int, memoryview], None],
        scale: int = 0,
        x: int = 0,
        y: int = 0,
        *,
        stride: int | None = None,
    ) -> None:
        """Decode the image that `open` accepted, once.

        The source is consumed, so call `open` again before the next decode.

        Args:
            target: A writable buffer of RGB565 pixels, row-major, or a
                callable. A callable is called as `callback(x, y, w, h, mv)`
                for each block of the image, in raster order, with `mv` a
                read-only memoryview of the block's `w * h` pixels. `mv` is
                valid only until the callback returns. Blocks are 8x8, 16x8
                or 16x16 pixels before scaling, clipped at the right and
                bottom edges.
            scale: 0 to 3, to shrink the image by 1, 2, 4 or 8. The decoded
                image is `(width >> scale)` by `(height >> scale)` pixels.
            x: Where the decoded image's left edge lands, 0 to 65535.
            y: Where its top edge lands, 0 to 65535.
            stride: For a buffer target, the pixels between the starts of
                two rows, which is the decoded width when None. Not accepted
                with a callable.

        Raises:
            ValueError: The buffer is too small for the image at `x`, `y` and
                `stride`, an argument is out of range, or the data is
                corrupt. A rejected call leaves the image open, so you can
                retry with a better target.
            RuntimeError: Nothing was opened.
        """

class JpegEncoder:
    """Encodes images as baseline JFIF JPEGs that any decoder reads.

    Encoding is in software on every port, and on the ESP32-P4 on the chip's
    JPEG engine unless you say otherwise. The two write different bytes for
    the same image; both decode to the source within the same quality.

    `quality`, `subsampling` and `hardware` are read-only properties.
    """

    quality: int
    """The quality passed to the constructor."""
    subsampling: int
    """420 or 444."""
    hardware: bool
    """Whether the last `encode` ran on the chip's JPEG engine."""

    def __init__(
        self,
        quality: int = 80,
        subsampling: int = 420,
        *,
        hardware: bool | None = None,
        exact: bool = True,
    ) -> None:
        """Make an encoder.

        Args:
            quality: 1 to 100. Higher is sharper and larger.
            subsampling: 420 halves the colour resolution each way, as
                cameras and the web do. 444 keeps it, at a larger size.
            hardware: None uses the JPEG engine where the chip has one and
                software elsewhere. False always uses software. True insists
                on the engine, and raises OSError on a chip without one.
            exact: Only the ESP32-P4's engine reads this. The engine widens
                RGB565 by filling the low bits with zeros, so full red reads
                as 248. True widens each pixel the way a display does first,
                so the JPEG matches the software encoder's. False has the
                engine read the buffer as it is: no copy, and about five
                times faster on a 720x720 frame, but red and blue run up to 7
                levels dark. That suits a camera stream.
        """
    def encode(
        self,
        buffer: bytes | bytearray | memoryview,
        width: int,
        height: int,
        *,
        format: int = RGB565,
        stride: int | None = None,
        swap: bool = False,
    ) -> bytes:
        """Compress an image.

        Args:
            buffer: `height` rows of `width` pixels.
            width: Pixels per row.
            height: Number of rows.
            format: `RGB565` or `GRAY`.
            stride: Bytes from the start of one row to the next. None means
                the rows follow each other with nothing between.
            swap: For RGB565, True when the bytes are in the swapped order
                many SPI displays keep.

        Returns:
            The JPEG file's bytes.
        """

def register_lvgl_decoder() -> bool:
    """Add this module's decoder to the running LVGL.

    After this, `lv.image` shows baseline JPEGs, from a variable holding the
    file's bytes or from a file, with one decoder in the firmware. It
    registers itself on `import jpegio` when LVGL is already initialised, so
    call it when `import jpegio` came before `lv.init()`, and after every
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
        For example `("jpegio", "LODEPNG", "BIN")` once this module's decoder
        is registered, and the empty tuple before `lv.init()` or in a
        firmware built without `lvgl`.
    """
