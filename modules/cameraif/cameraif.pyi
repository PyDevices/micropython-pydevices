# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Use a MIPI-CSI camera: live frames, stills, JPEG, and the picture controls.

`Camera` finds the sensor on the bus you name, starts it streaming and hands
you frames as memory views, bytes or scaled pictures in a display's buffer.
It runs on the ESP32-P4. On every other chip `available()` is False and
making a `Camera` raises `OSError`.

On an ESP32-P4 the module is also `espcamera`, with that module's `take()`,
`reconfigure()`, enums and properties, so CircuitPython code written for a
camera runs with only the constructor changed.

`capture_jpeg()` needs `jpegio` in the firmware, and `capture_scaled()`,
`capture_yuy2()`, `take()` at another size and GRAYSCALE need `ppa`.
"""

from typing import Any

__revision__: str
"""Which source this firmware was built from, from `git describe`: a release
tag, or the nearest one and the commit. "unknown" when the build had no git."""

def available() -> bool:
    """Say whether this firmware can open a camera at all.

    True when the build includes at least one sensor driver, False on every
    other chip and on a P4 build with none. It doesn't say whether a sensor is
    plugged in: making a `Camera` does that, with a message.
    """

class PixelFormat:
    """What `Camera.take()` returns. Small integers, as in `espcamera`."""

    RGB565: int
    """Two bytes a pixel."""
    GRAYSCALE: int
    """One byte of brightness a pixel."""
    JPEG: int
    """A JPEG-encoded picture."""

class FrameSize:
    """The size `Camera.take()` returns. Small integers, in order of size."""

    R96X96: int
    QQVGA: int
    QCIF: int
    HQVGA: int
    R240X240: int
    QVGA: int
    CIF: int
    HVGA: int
    VGA: int
    SVGA: int
    XGA: int
    HD: int
    SXGA: int
    UXGA: int
    FHD: int
    P_HD: int
    P_3MP: int
    QXGA: int
    QHD: int
    WQXGA: int
    P_FHD: int
    QSXGA: int

class GrabMode:
    """How `Camera.take()` picks a frame. Here only for code written for
    `espcamera`: a camera always gives the newest frame."""

    WHEN_EMPTY: int
    LATEST: int

class GainCeiling:
    """Here so code written for `espcamera` still imports. No sensor driver
    has a gain ceiling, so setting `Camera.gain_ceiling` always raises."""

    GAIN_2X: int
    GAIN_4X: int
    GAIN_8X: int
    GAIN_16X: int
    GAIN_32X: int
    GAIN_64X: int
    GAIN_128X: int

class Camera:
    """One camera on the CSI connector, streaming from the moment it exists.

    The chip has one CSI controller, so only one `Camera` can exist at a time;
    making a second raises. A `Camera` is a context manager that calls
    `deinit` on the way out.

    Besides the methods, a `Camera` has the properties `espcamera` has. The
    sensor's own controls (`vflip`, `hmirror`, `aec_value`, `agc_gain`,
    `brightness`, `contrast`, `saturation`, `sharpness`, `denoise`,
    `whitebal`, `gain_ctrl`, `exposure_ctrl`, `ae_level`, `wb_mode`,
    `special_effect`, `lenc` and `digital_gain`) read and write the sensor's own units, and
    raise `AttributeError` on a sensor whose driver lacks one; `controls()`
    says which it has.
    """

    pixel_format: int | None
    """What `take()` returns, a `PixelFormat`, or None for the sensor's own
    frame. Read-only; `reconfigure()` changes it."""
    frame_size: int
    """The size `take()` returns, a `FrameSize`. Read-only; `reconfigure()`
    changes it."""
    width: int
    """Width of what `take()` returns."""
    height: int
    """Height of what `take()` returns."""
    quality: int
    """`take()`'s JPEG quality, 1 to 100. Raises `ValueError` outside that."""
    colorbar: bool | int
    """The sensor's test pattern. Reads back what was last set: a bool, or a
    menu number above 1."""
    sensor_name: str
    """The sensor's name."""
    address: int
    """The sensor's address on the SCCB bus."""
    supports_jpeg: bool
    """True when `jpegio` is in the firmware and `take()` has a pixel format."""
    max_frame_size: int | None
    """The largest `FrameSize` the sensor's mode holds without scaling up."""
    frame_available: bool
    """True when a frame is waiting."""
    grab_mode: int
    """Always `GrabMode.LATEST`."""
    framebuffer_count: int
    """Always 2."""

    vflip: bool | None
    """Vertical flip, in the sensor. Reads back what was last set."""
    hmirror: bool | None
    """Horizontal mirror, in the sensor. Reads back what was last set."""
    aec_value: int | None
    """Exposure, in the sensor's units."""
    agc_gain: int | None
    """Gain, in the sensor's units."""
    brightness: int | None
    contrast: int | None
    saturation: int | None
    sharpness: int | None
    denoise: int | None
    whitebal: bool | None
    """Automatic white balance. On a sensor with none of its own, the ISP's."""
    gain_ctrl: bool | None
    exposure_ctrl: bool | None
    ae_level: int | None
    wb_mode: int | None
    special_effect: int | None
    lenc: bool | None
    digital_gain: int | None

    gain_ceiling: int
    """Always raises `AttributeError`: no sensor driver has one."""
    aec2: int
    """Always raises `AttributeError`."""
    awb_gain: int
    """Always raises `AttributeError`."""
    dcw: int
    """Always raises `AttributeError`."""
    bpc: int
    """Always raises `AttributeError`."""
    wpc: int
    """Always raises `AttributeError`."""
    raw_gma: int
    """Always raises `AttributeError`."""

    def __init__(
        self,
        sda: int,
        scl: int,
        *,
        i2c: Any = 0,
        ldo_chan: int = 3,
        ldo_mv: int = 2500,
        reset: int = -1,
        pwdn: int = -1,
        xclk: int = -1,
        xclk_hz: int = 24000000,
        format: str | int | tuple[int, int] | None = None,
        pixel_format: int | str | None = None,
        raw: bool = False,
        lane_mbps: int = 0,
        exposure: int | None = None,
        gain: int | None = None,
        flip: bool = False,
        mirror: bool = False,
        test_pattern: bool = False,
        bayer: str | None = None,
        awb: bool = True,
        auto_exposure: bool | None = None,
        black_level: int | None = None,
        sensor: dict[str, Any] | None = None,
        frame_size: int | None = None,
        jpeg_quality: int = 80,
        tuning: dict[str, Any] | None = None,
    ) -> None:
        """Find the sensor and start it streaming.

        Args:
            sda: The I2C data pin the sensor answers on.
            scl: The I2C clock pin.
            i2c: The I2C port, which must be the one that already owns those
                pins when the camera shares a bus with a touch controller or
                codec. On CircuitPython a `busio.I2C`, such as `board.I2C()`.
            ldo_chan: The LDO channel that powers the MIPI D-PHY.
            ldo_mv: That LDO's voltage in millivolts.
            reset: The sensor's reset pin, or -1 for none.
            pwdn: The sensor's power-down pin, or -1 for none.
            xclk: The pin to drive the sensor's clock on, or -1 when the
                camera module has its own oscillator.
            xclk_hz: The frequency of that clock.
            format: The sensor mode, by name, by index, or by size as
                `(width, height)`; None takes the driver's default.
            pixel_format: What `take()` returns, a `PixelFormat`; None
                returns the sensor's own frame. "rgb888" asks for 24-bit
                frames from `frame()` and `capture()`.
            raw: Skip the ISP and return the sensor's raw bytes.
            lane_mbps: Override the driver's MIPI lane rate; 0 trusts it.
            exposure: Starting exposure, 0 to 100 percent of the sensor's range.
            gain: Starting gain, 0 to 100 percent.
            flip: Start flipped vertically.
            mirror: Start mirrored horizontally.
            test_pattern: Start with the sensor's test pattern.
            bayer: The ISP's colour order, "BGGR", "GBRG", "GRBG" or "RGGB";
                None works it out from the sensor and the orientation.
            awb: Automatic white balance.
            auto_exposure: Steer exposure and gain toward a brightness
                target. On unless False.
            black_level: What the sensor reads with no light, in 8-bit steps.
            sensor: A sensor described in Python, used instead of probing.
            frame_size: The size `take()` returns, a `FrameSize`; None is the
                sensor's own.
            jpeg_quality: `take()`'s JPEG quality, 1 to 100.
            tuning: Colour tuning over the sensor's own, with the keys
                `tuning()` returns.

        Raises:
            OSError: No sensor answered, a camera already exists, or this
                chip has no MIPI-CSI port.
            ValueError: An argument is out of range or a sensor description
                is malformed.
        """
    def __enter__(self) -> "Camera": ...
    def __exit__(self, *exc: Any) -> None: ...

    def frame(self, timeout: int = 1000) -> memoryview | None:
        """Return the newest frame where it landed, with no copy.

        The view is borrowed: it's valid until your next `frame()` or
        `capture()`, so copy what you need to keep.

        Args:
            timeout: Milliseconds to wait for a frame.

        Returns:
            The frame, or None when none arrived in time.
        """
    def capture(self, buf: bytearray | memoryview | None = None, timeout: int = 1000) -> int | bytes:
        """Copy a frame out.

        Args:
            buf: A buffer at least as big as a frame (`size()` says), which
                is filled; None makes a new `bytes`.
            timeout: Milliseconds to wait for a frame.

        Returns:
            With a buffer, the number of bytes copied, or 0 when no frame
            arrived. Without, the frame as `bytes`.

        Raises:
            ValueError: The buffer is too small.
        """
    def capture_scaled(
        self,
        dst: bytearray | memoryview,
        pic_w: int,
        pic_h: int,
        *,
        x: int = 0,
        y: int = 0,
        w: int = 0,
        h: int = 0,
        rotate: int = 0,
        mirror: bool = False,
        src_rect: tuple[int, int, int, int] | None = None,
        swap: bool = False,
        timeout: int = 1000,
    ) -> tuple[int, int] | None:
        """Scale the next frame into a block of a picture, such as a panel's buffer.

        Needs `ppa` in the firmware. The destination is the whole picture plus
        the block to write inside it, so a preview can hand over its scanout
        buffer. It should start on a cache line, or the scale runs in software.

        Args:
            dst: The destination buffer, `pic_w` x `pic_h` RGB565.
            pic_w: The destination picture's width.
            pic_h: The destination picture's height.
            x: Left edge of the block to write.
            y: Top edge of the block to write.
            w: Width of the block; 0 fills to the picture's edge.
            h: Height of the block; 0 fills to the picture's edge.
            rotate: 0, 90, 180 or 270 degrees.
            mirror: Flip horizontally.
            src_rect: `(x, y, w, h)` of the part of the frame to use, for
                digital zoom.
            swap: Swap the two bytes of each pixel, for a panel that takes
                big-endian RGB565.
            timeout: Milliseconds to wait for a frame.

        Returns:
            The `(width, height)` written, or None when no frame arrived.
        """
    def capture_yuy2(self, buf: bytearray | memoryview, width: int, height: int, timeout: int = 1000) -> int:
        """Convert a frame to YUY2 at `width` x `height` into `buf`, as a USB webcam sends.

        Needs `ppa` in the firmware.

        Returns:
            The number of bytes written, or 0 when no frame arrived.
        """
    def capture_jpeg(self, quality: int = 80, timeout: int = 1000) -> bytes | None:
        """Encode a frame as a JPEG with the chip's hardware encoder.

        Needs `jpegio` in the firmware.

        Args:
            quality: 1 to 100; higher is sharper and larger.
            timeout: Milliseconds to wait for a frame.

        Returns:
            The JPEG, or None when no frame arrived.

        Raises:
            OSError: `jpegio` isn't in the firmware.
        """
    def take(self, timeout: float | None = 0.25) -> memoryview | Any | None:
        """Return the newest frame as `pixel_format` and `frame_size` say.

        The `espcamera` way. The frame is borrowed: it's valid until the next
        `take()`. In RGB565 and GRAYSCALE it's the pixels (a read-only
        `displayio.Bitmap` on CircuitPython, a `memoryview` on MicroPython);
        in JPEG it's a `memoryview` of the encoded bytes.

        Args:
            timeout: Seconds to wait.

        Returns:
            The frame, or None when none arrived in time.
        """
    def reconfigure(
        self,
        *,
        frame_size: int | None = None,
        pixel_format: int | None = None,
        grab_mode: int | None = None,
        framebuffer_count: int | None = None,
    ) -> None:
        """Change what `take()` returns on the running camera.

        `grab_mode` and `framebuffer_count` are accepted and ignored.

        Raises:
            ValueError: The camera was opened without a pixel format, or the
                pair isn't one `take()` can make.
        """

    def size(self) -> tuple[int, int, int]:
        """Return `(width, height, bytes_per_frame)` of the sensor's frame."""
    def sensor(self) -> tuple[str, int]:
        """Return `(name, product_id)` of the sensor."""
    def formats(self) -> list[tuple[str, int, int]]:
        """Return `(name, width, height)` for each mode the driver offers."""
    def controls(self) -> dict[str, tuple[int, int, int]]:
        """Return `{name: (minimum, maximum, step)}` for what this sensor's driver implements."""
    def exposure(self, pct: int | None = None, *, raw: bool = False) -> int | tuple[int, int, int, int] | None:
        """Read or set exposure as a percent of the sensor's range.

        Setting it turns `auto_exposure` off.

        Args:
            pct: The new percent, or None to read.
            raw: Use the sensor's own units. Read, this returns
                `(current, minimum, maximum, step)`.

        Returns:
            The value, or None when the driver can't say.
        """
    def gain(self, pct: int | None = None, *, raw: bool = False) -> int | tuple[int, int, int, int] | None:
        """Read or set gain as a percent of the sensor's range. Takes what `exposure` takes."""
    def flip(self, on: bool | None = None) -> bool | None:
        """Read or set the sensor's vertical flip."""
    def mirror(self, on: bool | None = None) -> bool | None:
        """Read or set the sensor's horizontal mirror."""
    def test_pattern(self, on: bool | int | None = None) -> bool | int:
        """Read or set the sensor's test pattern. Reads back what was last set.

        A bool turns the sensor's colour bars on or off. A number picks an
        entry from the menu the sensor's description lists: 0 is off, 1 the
        same as `True`, and `ValueError` is raised for a negative one.
        """
    def bayer(self) -> str:
        """Return the ISP's colour order the camera opened with."""
    def awb(self, on: bool | None = None) -> bool:
        """Read or set automatic white balance."""
    def wb_gains(self, red: float | None = None, blue: float | None = None) -> tuple[float, float]:
        """Read or set the white balance's red and blue gains (green is 1).

        Setting either turns `awb` off.
        """
    def auto_exposure(self, on: bool | None = None, *, target: int | None = None) -> tuple[bool, int, int]:
        """Steer exposure and gain toward a brightness target every frame.

        Args:
            on: Turn it on or off; None leaves it.
            target: The brightness to aim for, 0 to 255 after gamma.

        Returns:
            `(on, target, brightness)`.
        """
    def black_level(self, level: int | None = None) -> int:
        """Read or set what the sensor reads with no light, taken off before gamma."""
    def ccm(self, matrix: tuple[float, ...] | list[float] | None = None) -> tuple[float, ...]:
        """Read or set the colour matrix, nine numbers row by row."""
    def gamma(self, exponent: float | None = None) -> float:
        """Read or set the tone curve's exponent; below 1 lifts the mid-tones."""
    def color(
        self,
        contrast: int | None = None,
        saturation: int | None = None,
        brightness: int | None = None,
        hue: int | None = None,
    ) -> dict[str, int]:
        """Set any of the ISP's colour settings and return all four.

        Args:
            contrast: 0 to 255; 128 leaves it as is.
            saturation: 0 to 255; 0 is grey, 128 leaves it as is.
            brightness: -128 to 127, added.
            hue: 0 to 360 degrees round the colour wheel.

        Raises:
            ValueError: A value is out of range; nothing changes.
        """
    def tuning(self, **keys: Any) -> dict[str, Any]:
        """Return the sensor's colour tuning, changing any key you pass.

        The keys are `gamma`, `black_level`, `ccm`, `ccm_table`, `awb_table`
        and `color`. What it returns can be passed back as `tuning(**saved)`.
        """
    def color_temp(self) -> int | None:
        """Return the light's colour temperature in kelvin, or None without a matrix table."""
    def reg(self, addr: int, value: int | None = None) -> int | None:
        """Read one sensor register, or write it when you give a value."""
    def stats(self) -> tuple[int, int, int, int, int]:
        """Return `(frames, dropped, isr_new, isr_done, last_received)`.

        When there is no picture and `isr_done` isn't climbing, nothing is
        arriving: look at power, clock, lanes or the sensor's stream.
        """
    def deinit(self) -> None:
        """Stop the camera and free the CSI controller. Safe to call twice."""
