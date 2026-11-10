# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Scale, rotate, convert and fill pixel buffers.

`ppa` reshapes pictures in memory: it scales, rotates, mirrors and crops,
changes the pixel format, fills a block, and blends two pictures. On the
ESP32-P4 the chip's Pixel Processing Accelerator does the work, 6 to 30
times faster than the CPU. Everywhere else, desktop included, the same calls
run in portable C, so a program that uses `ppa` runs anywhere.

Buffers are anything with the buffer protocol: a `bytearray`, a `memoryview`,
a display framebuffer. Every function checks a buffer's length against the
picture it is said to hold and raises `ValueError` when it is short.

The PPA has rules, and a call it can't do runs in software unless you ask
for `hardware=True`: it scales only in steps of 1/16, and the destination
must start on a 64-byte boundary and hold the picture rounded up to the next
64 bytes.
"""

__revision__: str
"""Which source this firmware was built from, from `git describe`: a release
tag, or the nearest one and the commit. "unknown" when the build had no git."""

HARDWARE: bool
"""True when this chip has a PPA."""

RGB565: int
"""Pixel format: 16 bits, in the chip's native byte order, which is what
display framebuffers hold."""
RGB888: int
"""Pixel format: three bytes, B, G, R."""
ARGB8888: int
"""Pixel format: four bytes, B, G, R, A."""
GRAY8: int
"""Pixel format: one byte of BT.601 luma."""
YUY2: int
"""Pixel format: 4:2:2, two bytes a pixel in the order Y0 U Y1 V, what webcams
and USB video send."""
UYVY: int
"""Pixel format: 4:2:2, two bytes a pixel in the order U Y0 V Y1."""
YUV420: int
"""Pixel format: the ESP32-P4's packed 4:2:0, 12 bits a pixel (U Y Y on the
first line of each pair, V Y Y on the second), what its H.264 encoder reads."""

def srm(
    src: bytes | bytearray | memoryview,
    src_w: int,
    src_h: int,
    dst: bytearray | memoryview,
    dst_w: int,
    dst_h: int,
    *,
    src_fmt: int = RGB565,
    dst_fmt: int = RGB565,
    src_rect: tuple[int, int, int, int] | None = None,
    x: int = 0,
    y: int = 0,
    w: int = 0,
    h: int = 0,
    rotate: int = 0,
    mirror_x: bool = False,
    mirror_y: bool = False,
    swap: bool = False,
    limited: bool = False,
    approx: bool = False,
    hardware: bool | None = None,
) -> bool:
    """Scale, rotate and mirror in one operation, converting the format on the way.

    The whole source, or the `src_rect` block of it, is scaled to fill the
    `x, y, w, h` block of the destination. Pixels outside that block are left
    alone, which is how you put a picture in the middle of a bigger one.
    Scaling is nearest-neighbour and happens first, then the rotation, then
    the mirror. With a quarter turn the destination block is the turned
    shape: a 320x240 source turned 90 degrees fills a 240x320 block.

    Args:
        src: The source pixels.
        src_w: Source width in pixels.
        src_h: Source height in pixels.
        dst: The destination pixels.
        dst_w: Destination width in pixels.
        dst_h: Destination height in pixels.
        src_fmt: The source's pixel format.
        dst_fmt: The destination's pixel format.
        src_rect: `(x, y, w, h)` of the part of the source to use, or None
            for all of it.
        x: Left edge of the destination block.
        y: Top edge of the destination block.
        w: Width of the destination block, 0 for the whole picture.
        h: Height of the destination block, 0 for the whole picture.
        rotate: 0, 90, 180 or 270 degrees, counter-clockwise.
        mirror_x: Flip left to right.
        mirror_y: Flip top to bottom.
        swap: Read RGB565 as big-endian, the order many SPI panels' bytes
            are in.
        limited: Make YUV studio range (16 to 235), which is what a video
            encoder expects. Otherwise YUV is full range (0 to 255), which is
            what a JPEG holds.
        approx: Let the PPA take the nearest 1/16 step when the scale is not
            one, filling a little less of the block, instead of falling back
            to software.
        hardware: None uses the PPA when it can, False never does, and True
            insists, raising OSError with the reason when it can't.

    Returns:
        Whether the PPA did it.

    Raises:
        ValueError: A buffer is too short, or an argument is out of range.
        OSError: `hardware=True` and the PPA can't do this call.
    """
def convert(
    src: bytes | bytearray | memoryview,
    w: int,
    h: int,
    dst: bytearray | memoryview,
    src_fmt: int,
    dst_fmt: int,
    *,
    swap: bool = False,
    limited: bool = False,
    hardware: bool | None = None,
) -> bool:
    """Change a picture's pixel format, keeping its size.

    This is `srm` at the same size. The arguments mean what they mean there.

    Returns:
        Whether the PPA did it.
    """
def fill(
    dst: bytearray | memoryview,
    dst_w: int,
    dst_h: int,
    color: int,
    *,
    x: int = 0,
    y: int = 0,
    w: int = 0,
    h: int = 0,
    fmt: int = RGB565,
) -> None:
    """Fill a block with one colour.

    Only the ESP32-P4 has this. Elsewhere it raises OSError, and so does a
    `dst` that breaks the alignment rule.

    Args:
        dst: The picture to fill, in `fmt`.
        dst_w: Its width in pixels.
        dst_h: Its height in pixels.
        color: `0xRRGGBB`, or `0xAARRGGBB` for ARGB8888.
        x: Left edge of the block.
        y: Top edge of the block.
        w: Width of the block, 0 for the whole picture.
        h: Height of the block, 0 for the whole picture.
        fmt: `RGB565`, `RGB888` or `ARGB8888`.
    """
def blend(
    bg: bytes | bytearray | memoryview,
    fg: bytes | bytearray | memoryview,
    dst: bytearray | memoryview,
    w: int,
    h: int,
    alpha: int,
) -> None:
    """Draw one RGB565 picture over another.

    Only the ESP32-P4 has this, with `dst` aligned as the module description
    says. Elsewhere it raises OSError.

    Args:
        bg: The picture underneath.
        fg: The picture on top, the same size.
        dst: Where the result goes.
        w: Width of all three in pixels.
        h: Height of all three in pixels.
        alpha: 0 (all `bg`) to 255 (all `fg`).
    """
def size(fmt: int, w: int, h: int) -> int:
    """Say how many bytes a picture takes.

    Args:
        fmt: A pixel format.
        w: Width in pixels.
        h: Height in pixels.
    """
