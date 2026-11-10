# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""H.264 video on the ESP32-P4's hardware encoder.

`Encoder` turns RGB565 frames into H.264 access units on the chip's encoder.
Hand it any framebuffer; no camera or cast is involved. The colour conversion
to YUV 4:2:0 runs on the PPA, so firmware needs the `ppa` module as well.

Only the ESP32-P4 has the encoder. On every other chip and port the module is
not built, so `import h264enc` raises `ImportError`.
"""

__revision__: str
"""Which source this firmware was built from, from `git describe`: a release
tag, or the nearest one and the commit. "unknown" when the build had no git."""

class Encoder:
    """One H.264 encoding session.

    The P4 has one H.264 encoder, so one `Encoder` (or one cast) can be open at
    a time: a second raises `OSError` saying it is busy, and the first carries
    on. `close` gives the encoder back; so does the object being collected, and
    leaving a `with` block.

    With `pipeline=True` the colour conversion of one frame overlaps the encode
    of the one before. A frame then costs about the longer of the two instead
    of their sum, but each `encode` returns the previous frame's access unit
    (and `b""` the first time), and `flush` returns the last one.

    `keyframe`, `pipeline`, `width` and `height` are read-only properties.
    """

    keyframe: bool
    """Whether the access unit `encode` (or `flush`) last returned is an IDR
    frame."""
    pipeline: bool
    """Whether this is a pipelined encoder."""
    width: int
    """Frame width in pixels."""
    height: int
    """Frame height in pixels."""

    def __init__(
        self,
        width: int,
        height: int,
        fps: int = 30,
        gop: int = 30,
        bitrate: int = 3_000_000,
        qp_min: int = 10,
        qp_max: int = 45,
        *,
        canvas_w: int = 0,
        canvas_h: int = 0,
        out_size: int = 0,
        pipeline: bool = False,
    ) -> None:
        """Open the encoder.

        Args:
            width: Frame width, a multiple of 16, 80 to 1920.
            height: Frame height, a multiple of 16, 80 to 2032.
            fps: Frames per second.
            gop: Frames from one IDR (key) frame to the next.
            bitrate: Target bits per second.
            qp_min: Lowest quantiser, the best quality it may use.
            qp_max: Highest quantiser, the worst quality it may use.
            canvas_w: With `canvas_h`, a larger black picture to centre the
                frame on, which is how a cast puts a 720x720 panel on a
                1280x720 TV. 0 means no canvas.
            canvas_h: See `canvas_w`.
            out_size: The most bytes one access unit may take. 0 holds a
                keyframe of the whole canvas at its raw size, because a busy
                picture can encode bigger than any fixed guess, and a frame
                that doesn't fit stops the encoder.
            pipeline: Overlap the conversion and the encode, as above. It
                keeps two YUV canvases in PSRAM and starts one small task the
                first time it is used.

        Raises:
            ValueError: A size is out of range.
            OSError: The encoder is already open.
        """
    def encode(self, frame: bytes | bytearray | memoryview) -> bytes:
        """Encode one frame.

        Args:
            frame: Native-order RGB565 pixels, at least `width * height * 2`
                bytes. A pipelined encoder has finished reading it when this
                returns, so you can draw the next picture into the same
                buffer straight away.

        Returns:
            One access unit: Annex B NAL units, with the SPS and PPS before
            each IDR frame. A pipelined encoder returns the previous frame's
            access unit, and `b""` for the first.
        """
    def flush(self) -> bytes:
        """Return a pipelined encoder's last access unit, the one `encode`
        hasn't returned yet, or `b""` when there is none. A plain encoder
        always returns `b""`."""
    def force_idr(self) -> None:
        """Make the next frame an IDR frame, which a viewer joining late needs."""
    def set_bitrate(self, bps: int) -> None:
        """Change the target bitrate, from the next frame.

        Args:
            bps: Bits per second.
        """
    def stats(self) -> dict[str, int]:
        """Count what the encoder has done.

        Returns:
            A dict with `frames` and `keyframes` returned so far, `bytes` and
            `errors`, the last conversion's `ppa_us` and the last encode's
            `enc_us` in microseconds, and `wall_us`, how long the last
            `encode` kept its caller.
        """
    def close(self) -> bytes | None:
        """Give the encoder back.

        Returns:
            For a pipelined encoder, what `flush` would have returned. None
            for a plain one, or when it was already closed. Leaving a `with`
            block discards it, so flush first.
        """
    def __enter__(self) -> Encoder: ...
    def __exit__(self, *exc_info: object) -> None: ...
