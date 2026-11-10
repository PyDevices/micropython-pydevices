# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Record from an analog pin: CircuitPython's `analogbufio`, on an STM32H7 board.

It exists on the Arduino GIGA R1, where it records the headphone jack's
microphone ring. Only one of `BufferedIn` and `audiobusio.PDMIn` records at a
time.
"""

from typing import Any

class BufferedIn:
    """An ADC1 pin sampled on each tick of a timer, a few conversions averaged
    in hardware. While it records, `machine.ADC` on ADC1 has to wait."""

    sample_rate: int
    """The rate the timer really runs at, after the first `readinto()`."""

    def __init__(self, pin: Any, *, sample_rate: int) -> None:
        """Open the pin.

        Args:
            pin: An ADC1 pin, such as `PA0`.
            sample_rate: The rate to sample at, in hertz.
        """
    def __enter__(self) -> "BufferedIn": ...
    def __exit__(self, *exc: Any) -> None: ...
    def deinit(self) -> None:
        """Stop recording and release the pin."""
    def readinto(self, buffer: Any, *, loop: bool = False) -> int:
        """Fill the whole buffer with the ADC's own 16-bit values.

        Args:
            buffer: An `'H'` array for 16-bit values, or a `bytearray` or
                `'B'` array for their top bytes.
            loop: Raises `NotImplementedError` when True.

        Returns:
            The number of samples the buffer holds.

        Raises:
            ValueError: The buffer is another type.
        """
