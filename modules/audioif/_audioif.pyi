# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""The audio hardware under the pump: I2S channels, microphones and the DAC.

`_audioif` is the C half that `audiobusio`, `audioi2sin` and `audiodev` are
built on. A program uses those and doesn't import this module; this stub is for
the people who write them. Which names exist depends on the port: the ESP32 has
the I2S calls, Windows has the endpoint calls, the STM32H7 has the DAC and
capture calls, and every port has `driver()`. A port with no audio driver has no
`_audioif` at all.

The engine's own counters (`lock_stats`, `lock_reset`, `fault`) are on
`audiopump`.
"""

from typing import Any

__revision__: str
"""Which source this firmware was built from, from `git describe`: a release
tag, or the nearest one and the commit. "unknown" when the build had no git.
Not on the STM32H7 build."""

def driver() -> str:
    """Return the name of the driver this firmware bound: "esp32", "win32",
    "pthread" or "stm32". `audiopump.driver()` says the same."""

# --- ESP32 ---------------------------------------------------------------

def i2s_start(
    port: int,
    bclk: int,
    ws: int,
    dout: int,
    rate: int,
    *,
    bits: int = 16,
    channels: int = 2,
    mclk: int = -1,
    mclk_fs: int = 256,
    dma_desc: int = 6,
    dma_frame: int = 240,
    din: int = -1,
    in_port: int = -1,
    in_bclk: int = -1,
    in_ws: int = -1,
    in_mclk: int = -1,
) -> int:
    """ESP32. Open an I2S transmit channel that the pump writes every block into.

    Args:
        port: The I2S peripheral.
        bclk: The bit clock pin.
        ws: The word select pin.
        dout: The data out pin.
        rate: The sample rate.
        bits: Bits per sample.
        channels: 1 or 2.
        mclk: The master clock pin, or -1 for none.
        mclk_fs: Bit clocks of master clock per frame.
        dma_desc: How many DMA descriptors.
        dma_frame: Frames per descriptor.
        din: The data in pin; 0 or more opens the receive half of the same
            channel pair, so one clock tree drives capture and playback.
        in_port: With `din`, a different peripheral for the receive half, for a
            board whose microphone isn't on the speaker's port. The two aren't
            sample-locked.
        in_bclk: The receive half's bit clock pin, with `in_port`.
        in_ws: The receive half's word select pin, with `in_port`.
        in_mclk: The receive half's master clock pin, with `in_port`.

    Returns:
        The bytes the DMA holds when full, the block-to-wire latency floor.

    Raises:
        OSError: The channel wouldn't open.
    """

def i2s_stop() -> None:
    """ESP32. Close both channels, a recording included."""

def i2s_dma_bytes() -> int:
    """ESP32. Return the bytes the transmit DMA has clocked out, from its interrupt."""

def i2s_rx_bytes() -> int:
    """ESP32. Return the bytes the receive DMA has clocked in, from its interrupt."""

def i2s_sink_bytes() -> int:
    """ESP32. Return how far into the DMA's output stream the pump has
    accounted for: bytes handed over plus silence already charged as starvation."""

def i2s_starved_bytes() -> int:
    """ESP32. Return the bytes of silence the DMA sent because nothing was written."""

def i2s_state() -> tuple[int, int, bool, int] | None:
    """ESP32. Return `(rate, channels, has_rx, holders)`, or None with no channel open.

    `holders` is 1 for the output, 2 for the recorder, 3 for both.
    """

def rx_open(
    port: int,
    bclk: int,
    ws: int,
    dout: int,
    din: int,
    rate: int,
    mclk: int = -1,
    mclk_fs: int = 256,
    ring: int = 20000,
    take: int = 2,
) -> int:
    """ESP32. Record from the receive half while an output plays on the same port.

    Opens the channel pair if nothing has, or joins the one
    `I2SOut(..., data_in=)` opened. With `dout` -1 it opens a receive-only
    channel instead, as `audioi2sin.I2SIn` does.

    Args:
        port: The I2S peripheral.
        bclk: The bit clock pin.
        ws: The word select pin.
        dout: The speaker's data pin, or -1 for receive-only.
        din: The microphone's data pin.
        rate: The sample rate.
        mclk: The master clock pin, or -1 for none.
        mclk_fs: Bit clocks of master clock per frame.
        ring: Bytes of history the reader may fall behind by before audio is dropped.
        take: Which slot to read: 0 left, 1 right, 2 their average (one channel
            out), 3 both.

    Returns:
        The size of the capture ring in bytes.

    Raises:
        ValueError: A capture is already open, `take` isn't 0 to 3, `din` is
            negative, or `ring` is too small.
        OSError: The channel wouldn't open.
    """

def rx_read(buf: bytearray | memoryview) -> int:
    """Copy whole frames from the capture ring into `buf` without blocking.

    Returns:
        The bytes copied, or 0 when nothing is waiting.

    Raises:
        OSError: No capture is open.
    """

def rx_close() -> None:
    """Stop recording. The channel closes only if no output holds it."""

def rx_stats() -> tuple[int, int, int]:
    """Return `(frames captured, frames dropped, bytes waiting)`.

    Dropped frames are a reader that fell further behind than the ring.
    """

class Input:
    """A live microphone as an audio sample the pump pulls, so an effect graph
    can be built on it. The read blocks, which paces the pump. On Windows it
    reads a WASAPI microphone.

    It has the audio sample properties `sample_rate`, `bits_per_sample` and
    `channel_count`.
    """

    sample_rate: int
    bits_per_sample: int
    channel_count: int

    def __init__(
        self,
        *,
        sample_rate: int = 48000,
        channel_count: int = 2,
        frames: int = 256,
        timeout_ms: int = 500,
        device: str | None = None,
        buffer_ms: int = 100,
        keep_ms: int = 10,
        ring: Any = None,
    ) -> None:
        """Make the microphone sample.

        Args:
            sample_rate: The rate to read at.
            channel_count: 1 or 2.
            frames: Frames per block.
            timeout_ms: Milliseconds a read waits before it gives silence.
            device: Windows only: the endpoint ID of the microphone; None is
                the default one.
            buffer_ms: Windows only: the WASAPI buffer, in milliseconds.
            keep_ms: Windows only: milliseconds of audio kept ahead.
            ring: Windows only: an `audiopump.Ring` that a reader thread fills
                instead of the pump pulling blocks.

        Raises:
            ValueError: `channel_count` or `frames` is out of range.
        """
    def stats(self) -> tuple[int, ...]:
        """Return what it has read and missed.

        On an ESP32, `(blocks, timeouts, short_reads, block_bytes)`. On
        Windows, `(frames, short reads, glitches, dropped, timeouts, buffer
        frames, frames waiting, error, period frames, latency frames)`.
        """
    def deinit(self) -> None:
        """Windows only. Stop capturing and let the device go."""

def rt_probe(
    capture: bytearray | memoryview,
    frames: int = 256,
    lead: int = 0,
    prime: int = 0,
    level: int = 24000,
    channels: int = 2,
    timeout_ms: int = 2000,
) -> tuple[int, int, int, int, int, int]:
    """ESP32. Measure the round trip from the speaker to the microphone, in frames.

    It loads a click into the transmit DMA before either channel is enabled and
    captures the receive side from the same instant. Run it with no pump
    spawned.

    Args:
        capture: A buffer for the captured bytes.
        frames: Frames in the click block.
        lead: Frames of silence before the step.
        prime: Blocks of silence ahead of the click, to model a full ring.
        level: The step's level.
        channels: 1 or 2.
        timeout_ms: Milliseconds to wait for the capture.

    Returns:
        `(bytes captured, frame the step leaves the DAC, frames loaded,
        microseconds between enabling RX and TX, error code, frame bytes)`.
    """

def lock_probe() -> tuple[int, int, int]:
    """ESP32. Ask FreeRTOS whether it refuses a give of the pump mutex from a
    task that doesn't own it.

    Returns:
        `(held, freed, owner)`: another task gives a mutex the interpreter
        holds, then one nobody holds, then a task gives back one it took. The
        last must be 1.
    """

# --- Windows -------------------------------------------------------------

def sink_stats() -> tuple[int, ...] | None:
    """Windows. Return the WASAPI speaker's counters while one is open, in the
    same order as `Input.stats()`, or None when the sink isn't a speaker."""

def endpoint(device: str | None = None, capture: bool = False) -> tuple[str, str, str]:
    """Windows. Describe an audio endpoint.

    Args:
        device: The endpoint ID; None is the default one.
        capture: True for a microphone, False for a speaker.

    Returns:
        `(id, name, container)`: the endpoint's ID, its friendly name and the ID
        of the physical device it belongs to.

    Raises:
        OSError: No such endpoint.
    """

def same_clock(speaker: str | None = None, mic: str | None = None) -> bool:
    """Windows. Say whether a speaker and a microphone run on one clock.

    True when both are endpoints of the same physical device. The built-in
    endpoints share a container whatever their hardware, so two of them count
    as two clocks.

    Raises:
        OSError: No such endpoint.
    """

# --- STM32H7 -------------------------------------------------------------

def dac_stats() -> tuple[int, int, int, int, int]:
    """STM32H7. Return `(written, played, starved frames, service errors, ring frames)`."""

def render_mode(mode: str | None = None) -> str:
    """STM32H7. Read or choose where the pump renders.

    Args:
        mode: "irq" renders from an interrupt of the driver's own;
            "interpreter" renders on the interpreter, between bytecodes. None
            only reads. The choice lasts until the board resets.

    Returns:
        The mode now in force.

    Raises:
        ValueError: The mode is neither, or "irq" on an audiodsp that can't
            render from an interrupt.
    """

def render_stats(reset: bool = False) -> tuple[int, int, int, int, int, int]:
    """STM32H7. Return `(renders, longest render in microseconds, render stack
    used in bytes, render stack size, lock depth, interrupt priority)`.

    Args:
        reset: Zero the first two after reading them.
    """

def pdm_open(clock: Any, data: Any, rate: int, *, edge: int = 0) -> int:
    """STM32H7. Record a PDM microphone through DFSDM1 into the capture ring, as
    signed 16-bit mono.

    Args:
        clock: The PDM clock pin.
        data: The PDM data pin.
        rate: The rate to record at.
        edge: The clock edge the data is read on, 0 rising and 1 falling,
            which follows how the microphone's L/R pin is tied.

    Returns:
        The rate it really runs at.
    """

def adc_open(pin: Any, rate: int, *, raw: bool = False) -> int:
    """STM32H7. Record an ADC1 pin, paced by a timer, into the capture ring.

    Args:
        pin: The ADC1 pin.
        rate: The rate to record at.
        raw: Give the ADC's unsigned values, with no high-pass or gain.

    Returns:
        The rate it really runs at.
    """

def rx_gain(db: float | None = None) -> float:
    """STM32H7. Read the capture's gain in decibels, or set it from -40 to 60.

    0 dB puts the input's full scale at int16's. It holds across recordings.
    """

def rx_rate() -> int:
    """STM32H7. Return the rate the recording runs at, or 0 with none."""
