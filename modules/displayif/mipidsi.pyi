# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Drive a MIPI DSI panel from a framebuffer, with CircuitPython's ``mipidsi`` names.

Built on the ESP32-P4, the MIMXRT1176 and the STM32H747/H757; elsewhere the
module imports and its constructors raise `NotImplementedError`. A
`Display` is a buffer of RGB565 pixels that displaydev's ``FBDisplay``
draws into.
"""

from typing import Any

__revision__: str

class Bus:
    """The DSI host and its PHY.

    Args:
        frequency: The bit rate of each data lane, in Hz.
        num_lanes: The number of data lanes, 1 to 4 (1 or 2 on the
            STM32H747).
        ldo_chan: ESP32-P4 only: the LDO channel that powers the DSI PHY.
        ldo_voltage_mv: ESP32-P4 only: that LDO's voltage, in millivolts.
    """

    def __init__(
        self,
        *,
        frequency: int = 500_000_000,
        num_lanes: int = 2,
        ldo_chan: int = 3,
        ldo_voltage_mv: int = 2500,
    ) -> None: ...
    def deinit(self) -> None:
        """Release the DSI host, and any `Display` on it."""

class Display:
    """A DSI panel in video mode, scanning a framebuffer.

    Supports the buffer protocol: the bytes are the framebuffer, RGB565,
    ``row_stride`` bytes per row.

    Args:
        bus: The `Bus` the panel is on.
        init_sequence: The panel's start-up commands: each is a command
            byte, a parameter count (``| 0x80`` when a delay in milliseconds
            follows the parameters, 255 meaning 500 ms), then the parameters.
        width: The panel's width in pixels.
        height: The panel's height in pixels.
        hsync_pulse_width: Horizontal sync, in pixels.
        hsync_back_porch: Horizontal back porch, in pixels.
        hsync_front_porch: Horizontal front porch, in pixels.
        vsync_pulse_width: Vertical sync, in lines.
        vsync_back_porch: Vertical back porch, in lines.
        vsync_front_porch: Vertical front porch, in lines.
        pixel_clock_frequency: The pixel clock, in Hz. The STM32H747 uses
            the nearest it can make, and reports it as
            ``pixel_clock_frequency``.
        virtual_channel: The DSI virtual channel, 0 to 3 on the ESP32-P4;
            0 elsewhere.
        color_depth: Bits per pixel; only 16 is supported.
        non_burst: ESP32-P4 only: non-burst video with sync pulses.
        continuous_clock: ESP32-P4 only: keep the clock lane in high-speed
            mode between lines.
    """

    width: int
    height: int
    row_stride: int
    pixel_clock_frequency: int
    """STM32H747 only: the pixel clock the display runs at, in Hz."""

    def __init__(
        self,
        bus: Bus,
        init_sequence: Any,
        *,
        width: int,
        height: int,
        hsync_pulse_width: int,
        hsync_back_porch: int,
        hsync_front_porch: int,
        vsync_pulse_width: int,
        vsync_back_porch: int,
        vsync_front_porch: int,
        pixel_clock_frequency: int,
        virtual_channel: int = 0,
        color_depth: int = 16,
        non_burst: bool = False,
        continuous_clock: bool = False,
    ) -> None: ...
    def refresh(self, *args: Any) -> None:
        """Present what has been drawn into the framebuffer."""
    def refresh_rect(self, x: int, y: int, w: int, h: int) -> None:
        """ESP32-P4 only: present the rows ``y`` to ``y + h`` alone."""
    def blit(self, buf: Any, x: int, y: int, w: int, h: int) -> None:
        """ESP32-P4 and STM32H747: copy ``w`` x ``h`` RGB565 pixels from ``buf`` to (``x``, ``y``)."""
    def fill_rect(self, x: int, y: int, w: int, h: int, color: int) -> None:
        """Fill a rectangle with one RGB565 colour."""
    def deinit(self) -> None:
        """Stop scanning out. Drawing into the display afterwards raises."""
