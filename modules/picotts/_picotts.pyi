# SPDX-FileCopyrightText: 2026 PyDevices
#
# SPDX-License-Identifier: MIT
"""The SVOX Pico speech engine, as a C module. Use `picotts.TTS` rather than this
module: it handles the voice files, the speech buffer and the audio output."""

from typing import Final

__revision__: Final[str]
"""Which source the firmware was built from, or ``"unknown"`` for a build with no git."""

SAMPLE_RATE: Final = 16000
"""Rate of the speech the engine renders, in Hz."""

def psram() -> int:
    """Bytes of PSRAM the board has, ``0`` if it has none, or ``-1`` on a desktop build."""
    ...

def samples(n: int) -> memoryview:
    """Allocate ``n`` signed 16-bit samples, zeroed, as one block.

    Raises `ValueError` when ``n`` is not above zero and `MemoryError` when the heap has
    no block that large.
    """
    ...

class Engine:
    """One speech engine with one voice loaded. It reads the voice buffers, its work
    area and the current text in place, so keep them alive until `deinit`."""

    speaking: bool
    """True while text is waiting to be rendered or rendered samples are still held."""

    def __init__(self, ta: bytes | bytearray | memoryview, sg: bytes | bytearray | memoryview, memory: bytearray | memoryview) -> None:
        """Load a voice.

        Args:
            ta: The contents of the voice's text-analysis file (``en_US_ta.bin``).
            sg: The contents of its signal-generation file (``en_US_lh0_sg.bin``).
            memory: The engine's work area, at least 1,100,000 bytes.

        Raises `ValueError` for a work area that is too small and `RuntimeError` for
        voice files that aren't whole.
        """
        ...

    def start(self, text: bytes | bytearray | memoryview) -> None:
        """Start rendering ``text``, which must end in a NUL byte. Drops any text not yet
        rendered."""
        ...

    def stop(self) -> None:
        """Drop any text not yet rendered."""
        ...

    def render(self, out: bytearray | memoryview) -> int:
        """Render speech into ``out``, a writable buffer of signed 16-bit samples, and
        return how many samples were written. Fewer than ``out`` holds only when the
        text is finished."""
        ...

    def deinit(self) -> None:
        """Release the engine and drop its buffers. Safe to call twice."""
        ...
