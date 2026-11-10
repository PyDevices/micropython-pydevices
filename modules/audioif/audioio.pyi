# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Play audio through a DAC: CircuitPython's `audioio`, on an STM32H7 board.

It exists on boards whose audio is a DAC rather than I2S, such as the Arduino
GIGA R1's headphone jack. Elsewhere there is no `audioio`; use
`audiobusio.I2SOut`.
"""

from typing import Any

class AudioOut:
    """The DAC output. It has everything `audiobusio.I2SOut` has after its
    constructor: `play`, `retarget`, `stop`, `pause`, `resume`, `deinit`,
    `starved` and `sink_timeouts`, and the properties `playing`, `paused` and
    `status`.

    Only one output can exist at a time; a second raises
    `RuntimeError("Peripheral in use")`.
    """

    playing: bool
    """True while a sample plays, paused or not."""
    paused: bool
    """True after `pause()` and before `resume()`."""
    status: bytearray
    """The audio pump's status words, which say why it stopped."""

    def __init__(
        self,
        left_channel: Any,
        *,
        right_channel: Any = None,
        quiescent_value: int = 0x8000,
        sample_rate: int = 22050,
        buffer_ms: int = 0,
    ) -> None:
        """Open the DAC.

        Args:
            left_channel: The pin of the left DAC channel, such as `PA5`.
            right_channel: The pin of the right channel, such as `PA4`; None for
                one channel.
            quiescent_value: The level the output rests at. Only the default,
                0x8000 (midscale), is supported.
            sample_rate: The rate the DAC opens at, before the first `play()`.
            buffer_ms: Milliseconds of audio the DAC's ring holds; 0 is about
                93. More rides out longer stalls and puts the sound further
                behind.

        Raises:
            NotImplementedError: `quiescent_value` isn't 0x8000.
            ValueError: `buffer_ms` is negative.
            RuntimeError: An output already exists.
        """
    def __enter__(self) -> "AudioOut": ...
    def __exit__(self, *exc: Any) -> None: ...
    def deinit(self) -> None:
        """Stop playing and release the DAC."""
    def play(self, sample: Any, *, loop: bool = False) -> None:
        """Start playing a sample, stopping any that plays. Takes a `RawSample`
        or a `WaveFile`."""
    def retarget(self, sample: Any, *, loop: bool = False) -> None:
        """Swap what's playing for another sample without a gap."""
    def stop(self) -> None:
        """Stop playing."""
    def pause(self) -> None:
        """Pause playback."""
    def resume(self) -> None:
        """Carry on from where `pause()` stopped."""
    def starved(self) -> int:
        """Return the bytes of silence the DAC played because the pump hadn't
        written them in time, since `play()`."""
    def sink_timeouts(self) -> int:
        """Return how many blocks the sink refused because the ring had no room."""
