# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""PNG encoding and decoding.

`PngEncoder` turns an RGB565, greyscale or RGB888 buffer into a compressed PNG
and `PngDecoder` reads one back into RGB565, a row at a time, so a decode
holds two rows and a 32 KB window rather than the whole image. A 480x270
screen of flat panels encodes to about 3 KB.

The same code runs on every MicroPython port. On CPython, the `pngio` module
in the desktop package gives the same API over Pillow.
"""

__revision__: str
"""Which source this firmware was built from, from `git describe`: a release
tag, or the nearest one and the commit. "unknown" when the build had no git."""

RGB565: int
"""`PngEncoder.encode` format: two bytes per pixel, in the chip's native byte
order. What `PngDecoder.decode` writes."""
GS8: int
"""`PngEncoder.encode` format: one byte of grey per pixel."""
RGB888: int
"""`PngEncoder.encode` format: three bytes per pixel, red, green, blue."""

class PngEncoder:
    """Compresses images to PNG.

    Each row takes the PNG filter whose output has the smallest sum, then the
    image is one deflate stream. Keep one encoder and reuse it: its scratch
    memory stays allocated between frames.
    """

    def __init__(self, level: int = 1) -> None:
        """Make an encoder.

        Args:
            level: 1 (fastest) to 9 (smallest).
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

        RGB565 becomes 8-bit colour by repeating bits, so 31 becomes 255.

        Args:
            buffer: The pixels, row-major.
            width: Pixels per row.
            height: Number of rows.
            format: `RGB565`, `GS8` or `RGB888`.
            stride: Pixels from the start of one row to the next, to encode
                a block of a bigger picture. None means the rows follow each
                other with nothing between.
            swap: For RGB565, True reads the bytes big-endian.

        Returns:
            The PNG file's bytes.

        Raises:
            ValueError: The buffer is too small, or an argument is out of range.
        """

class PngDecoder:
    """Reads PNGs into RGB565.

    It reads 8-bit grey, RGB, palette, grey with alpha and RGBA, and 1, 2 and
    4-bit grey and palette. Alpha is dropped. Interlaced PNGs and 16-bit
    depth are refused. A corrupt image is caught by its checksum.

    Decoding inflates through MicroPython's `deflate` module, so a firmware
    without it raises OSError from `decode`.

    `width` and `height` are properties, valid after a successful `open`.
    """

    width: int
    """Width in pixels of the image the last `open` accepted."""
    height: int
    """Height in pixels of the image the last `open` accepted."""

    def __init__(self) -> None: ...
    def open(self, source: str | bytes | bytearray | memoryview | object) -> tuple[int, int]:
        """Read a PNG's header and return its size.

        Args:
            source: A path, a bytes-like object, or a binary stream.

        Returns:
            (width, height) in pixels.

        Raises:
            ValueError: The data is not a PNG this module reads.
        """
    def decode(
        self,
        target: bytearray | memoryview,
        x: int = 0,
        y: int = 0,
        *,
        stride: int | None = None,
    ) -> None:
        """Write the image that `open` accepted into a buffer, once.

        Pixels of the buffer outside the image are left alone. Call `open`
        again before the next decode.

        Args:
            target: A writable buffer of native-order RGB565 pixels.
            x: Where the image's left edge lands.
            y: Where its top edge lands.
            stride: Pixels from the start of one row of the target to the
                next, which is the image's width when None.

        Raises:
            ValueError: The target is too small, or the data is corrupt.
            OSError: The firmware has no `deflate` module.
        """
