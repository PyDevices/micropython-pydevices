# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Play audio out of an I2S bus: CircuitPython's `audiobusio`, over the audio pump.

`I2SOut` plays an audio sample, such as a `RawSample`, a `WaveFile` or a
synth, through an I2S amplifier or codec. A program written for CircuitPython's
`I2SOut` runs unchanged apart from its pins. `PDMIn` records from a PDM
microphone and exists on the STM32H7 only.

On a board whose audio is a DAC, such as the Arduino GIGA R1, `I2SOut` raises
`NotImplementedError`: use `audioio.AudioOut`.
"""

from typing import Any

class I2SOut:
    """One I2S output. Only one output can exist at a time; a second raises
    `RuntimeError("Peripheral in use")`.

    Calling `play` while playing stops the first sample. A sample ends by
    itself and `playing` goes False.
    """

    playing: bool
    """True while a sample plays, paused or not. Goes False by itself when a
    sample ends."""
    paused: bool
    """True after `pause()` and before `resume()`."""
    status: bytearray
    """The audio pump's status words, which say why it stopped. The same object
    every time."""

    def __init__(
        self,
        bit_clock: Any,
        word_select: Any,
        data: Any,
        *,
        main_clock: Any = None,
        left_justified: bool = False,
        external_clock: bool = False,
        port: int = -1,
        sample_rate: int = 48000,
        main_clock_fs: int = 256,
        sink: str | None = None,
        data_in: Any = None,
    ) -> None:
        """Open the bus.

        Args:
            bit_clock: The bit clock pin.
            word_select: The word select (left/right clock) pin.
            data: The data out pin.
            main_clock: The master clock pin, for a codec that needs one.
            left_justified: Raises `NotImplementedError` when True; the bus is
                Philips-mode.
            external_clock: Raises `NotImplementedError` when True.
            port: Which I2S peripheral; -1 lets the driver pick. Needed when a
                board's codec and microphone are on different ones.
            sample_rate: The rate the bus opens at, before the first `play()`
                retunes it to the sample's.
            main_clock_fs: Bit clocks of master clock per frame.
            sink: Desktop only: the file the blocks are written to, or a
                "wasapi" speaker on Windows.
            data_in: The microphone's data pin on a board whose microphone
                shares this output's clocks. The channel then opens as a
                transmit and receive pair, so `_audioif.rx_open` can record
                while this plays.

        Raises:
            NotImplementedError: `left_justified` or `external_clock`, or the
                board's audio is a DAC.
            RuntimeError: An output already exists.
        """
    def __enter__(self) -> "I2SOut": ...
    def __exit__(self, *exc: Any) -> None: ...
    def deinit(self) -> None:
        """Stop playing and release the bus."""
    def play(self, sample: Any, *, loop: bool = False) -> None:
        """Start playing a sample, stopping any that plays.

        Args:
            sample: An audio sample: a `RawSample`, a `WaveFile`, a `Mixer` or
                anything else that's an `audiosample`.
            loop: Start the sample over when it ends.

        Raises:
            ValueError: The sample has no frames, or its format can't be played.
        """
    def retarget(self, sample: Any, *, loop: bool = False) -> None:
        """Swap what's playing for another sample without a gap: no stop, and
        no channel close and reopen.

        Raises:
            RuntimeError: Nothing is playing.
            ValueError: The new sample's format differs from what's playing.
        """
    def stop(self) -> None:
        """Stop playing."""
    def pause(self) -> None:
        """Pause, leaving the bus clocking zeros.

        Raises:
            RuntimeError: Nothing is playing.
        """
    def resume(self) -> None:
        """Carry on from where `pause()` stopped."""
    def starved(self) -> int:
        """Return the bytes of silence the DMA played because the pump hadn't
        written them in time, since `play()`. It never goes down, and is 0 on a
        clean playback."""
    def sink_timeouts(self) -> int:
        """Return how many blocks the sink refused because the DMA had no room: the
        pump is ahead, the opposite of `starved()`."""

class PDMIn:
    """A PDM microphone, recorded through the STM32H7's DFSDM filter.

    On every other build, making one raises `NotImplementedError`.
    """

    sample_rate: int
    """The rate it really runs at: the nearest the filter can divide to."""

    def __init__(
        self,
        clock_pin: Any,
        data_pin: Any,
        *,
        sample_rate: int = 16000,
        bit_depth: int = 8,
        mono: bool = True,
        oversample: int = 64,
        startup_delay: float | None = None,
    ) -> None:
        """Start the microphone.

        Args:
            clock_pin: The PDM clock pin.
            data_pin: The PDM data pin.
            sample_rate: The rate to record at. 8, 16, 24, 32 and 48 kHz are exact.
            bit_depth: 8 or 16.
            mono: Must be True; the board has one microphone.
            oversample: Checked to be a multiple of 8 and otherwise unused: the
                decimation follows the rate.
            startup_delay: Seconds to wait for the microphone to settle, 0 to 1;
                None waits 0.11.

        Raises:
            ValueError: A setting isn't one the board supports.
            RuntimeError: A recording is already open.
        """
    def __enter__(self) -> "PDMIn": ...
    def __exit__(self, *exc: Any) -> None: ...
    def deinit(self) -> None:
        """Stop the microphone."""
    def record(self, destination: Any, destination_length: int) -> int:
        """Fill `destination` with samples.

        Args:
            destination: A `bytearray` or `'B'` array at 8 bits, or an `'H'`
                array at 16 bits. A signed `'b'` or `'h'` array takes signed
                samples.
            destination_length: How many samples to record.

        Returns:
            The number of samples written.

        Raises:
            ValueError: The length is negative or the buffer is too small or
                the wrong type.
        """
