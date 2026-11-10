# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Record from an I2S microphone: CircuitPython's `audioi2sin`.

`I2SIn` records from a MEMS microphone, or from a codec's ADC that the board
has set up first. A program written for CircuitPython's `I2SIn` runs
unchanged apart from its pins.
"""

from typing import Any

class I2SIn:
    """One I2S microphone. Only one can exist at a time; a second raises
    `RuntimeError("Peripheral in use")`.

    `record` reads from a quarter-second ring the DMA fills, so a pause between
    calls (a garbage collection, a file write) loses nothing and successive
    calls are continuous.
    """

    sample_rate: int
    """The rate it records at."""
    bits_per_sample: int
    """The width of the samples `record` writes."""
    channel_count: int
    """1 for mono, 2 for stereo."""
    bit_depth: int
    """The width of the samples on the wire."""
    samples_signed: bool
    """True when the samples are signed."""
    overflow: bool
    """True when the ring dropped samples since you last looked."""

    def __init__(
        self,
        bit_clock: Any,
        word_select: Any,
        data: Any,
        *,
        main_clock: Any = None,
        sample_rate: int = 16000,
        bit_depth: int = 16,
        output_bit_depth: int | None = None,
        mono: bool = True,
        left_justified: bool = False,
        samples_signed: bool = True,
        external_clock: bool = False,
        port: int = -1,
        main_clock_fs: int = 256,
        source: bytes | bytearray | memoryview | None = None,
    ) -> None:
        """Open the microphone.

        Args:
            bit_clock: The bit clock pin.
            word_select: The word select pin.
            data: The data in pin.
            main_clock: The master clock pin, for a codec that needs one.
            sample_rate: The rate to record at.
            bit_depth: 8, 16, 24 or 32. A 24-bit microphone is read from the
                top of its 32-bit slot.
            output_bit_depth: The width of the samples written; None uses
                `bit_depth`.
            mono: Record the left slot only.
            left_justified: Use left-justified rather than Philips framing.
            samples_signed: Write signed samples.
            external_clock: Raises `NotImplementedError` when True.
            port: Which I2S peripheral; -1 takes the first free one.
            main_clock_fs: Bit clocks of master clock per frame.
            source: On a build with no I2S: a buffer of two-slot wire frames
                that `record` reads in a loop. Without it a desktop records
                silence, paced at the sample rate.

        Raises:
            ValueError: A setting isn't one that's supported.
            NotImplementedError: `external_clock`.
            RuntimeError: A microphone already exists.
        """
    def __enter__(self) -> "I2SIn": ...
    def __exit__(self, *exc: Any) -> None: ...
    def deinit(self) -> None:
        """Stop recording and release the bus."""
    def record(self, destination: Any, destination_length: int) -> int:
        """Fill `destination` with samples.

        Args:
            destination: An array whose typecode fits the bit depth.
            destination_length: How many samples to record.

        Returns:
            The number of samples written. It's short when the microphone
            delivered nothing for a second.

        Raises:
            ValueError: The length is negative or the buffer is too small.
            TypeError: The buffer's typecode doesn't fit the bit depth.
        """
