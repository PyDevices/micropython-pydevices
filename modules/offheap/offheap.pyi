# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Keep big byte buffers out of the garbage-collected heap.

MicroPython's garbage collector reads every word of every `bytearray` a
program holds, in case one of them is a pointer. A few megabytes of pixels
make each collection take a tenth of a second or more, and an animation
freezes while it runs. A `Buffer` holds its bytes outside the heap, so the
collector skips them.

Use it where you would use `bytearray(size)` for a frame, a sprite sheet or
an off-screen background. It has the buffer protocol, so pygraphics'
`FrameBuffer`, `framebuf`, `memoryview` and the display drivers take it as
they take a `bytearray`.

On ESP32 the bytes come from PSRAM (internal RAM on a board without it); on
the unix, Windows and browser builds they come from C's `malloc`. Other ports
give all their RAM to the garbage collector, so they have no `offheap`.
"""

from types import TracebackType

__revision__: str
"""Which source this firmware was built from, from `git describe`: a release
tag, or the nearest one and the commit. "unknown" when the build had no git."""

class Buffer:
    """`size` zeroed bytes outside the garbage-collected heap.

    The bytes are freed by `deinit`, when the `Buffer` itself is collected,
    or at a soft reset, whichever comes first.

    Keep the `Buffer` for as long as anything draws into it. A `memoryview`
    of it, or a driver that took its address, doesn't keep it alive: the
    collector can't see a reference into memory it doesn't manage, so once
    the `Buffer` is unreachable its bytes can be freed under them.
    `framebuf.FrameBuffer` and pygraphics' `FrameBuffer` keep a reference to
    the object they were given, so a frame made from a `Buffer` keeps it.

    Example:
        ```python
        import offheap
        from pygraphics import RGB565, FrameBuffer

        pixels = offheap.Buffer(320 * 240 * 2)
        frame = FrameBuffer(pixels, 320, 240, RGB565)
        frame.fill(0x001F)
        ```

    Args:
        size: How many bytes, 0 or more.

    Raises:
        ValueError: `size` is negative.
        MemoryError: There isn't `size` bytes free outside the heap, even
            after a collection has freed the `Buffer`s nobody holds.
    """

    def __init__(self, size: int, /) -> None: ...
    def deinit(self) -> None:
        """Free the bytes now. Calling it again does nothing.

        Anything still pointing at the bytes, a `memoryview` or a frame
        made from this `Buffer`, must not be used afterwards. Using the
        `Buffer` itself raises `ValueError`.
        """

    def __len__(self) -> int:
        """The size in bytes. Raises `ValueError` after `deinit`."""

    def __bool__(self) -> bool: ...
    def __buffer__(self, flags: int, /) -> memoryview:
        """The bytes, readable and writable. Raises `ValueError` after `deinit`."""

    def __enter__(self) -> "Buffer": ...
    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        """Call `deinit`."""

def mem_alloc() -> int:
    """How many bytes live `Buffer`s hold, the counterpart of `gc.mem_alloc`."""
