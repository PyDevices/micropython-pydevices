# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Live video from an ESP32-P4 to a screen across the room.

Hand `Hls` or `Cast` an RGB565 framebuffer. A FreeRTOS task on core 0 scales
and converts it with the PPA, encodes H.264 in hardware, muxes MPEG-TS and
sends it, so your program keeps core 1 and its frame rate, and a long redraw
can't starve the stream.

There are two ways out:

- `Hls` serves HLS over HTTP, for anything that plays it: VLC, ffplay, a
  browser with hls.js, a Roku.
- `Cast` sends RTP to a Miracast (Wi-Fi Display) receiver, such as a TV, once
  your code has set up the session.

The encoding and the muxing are the `h264enc` and `tsmux` modules', so a
firmware needs all of castif, h264enc, tsmux and ppa; without one of them,
`Cast()` and `Hls()` say which is missing. Only the ESP32-P4 has castif.
"""

__revision__: str
"""Which source this firmware was built from, from `git describe`: a release
tag, or the nearest one and the commit. "unknown" when the build had no git."""

class Hls:
    """Serves what you draw as live HLS over HTTP.

    `start` serves `/stream.m3u8` and its `seg<N>.ts` segments. Call `offer`
    after each frame you draw. The task forces a keyframe every `target`
    milliseconds and cuts a segment at each one. It keeps the last eight
    segments in PSRAM and lists the newest `segments` in the playlist.

    Only one H.264 encoder exists, so a second `Hls`, a `Cast` or an
    `h264enc.Encoder` raises `OSError` while another has it.
    """

    def __init__(
        self,
        width: int,
        height: int,
        *,
        fps: int = 20,
        bitrate: int = 2_000_000,
        segments: int = 3,
        target: int = 900,
        port: int = 8090,
        path: str | None = None,
    ) -> None:
        """Open the encoder and the buffers. Nothing is served until `start`.

        Args:
            width: Frame width, a multiple of 16.
            height: Frame height, a multiple of 16.
            fps: Frames per second, 1 to 60.
            bitrate: Target bits per second.
            segments: How many segments the playlist lists, 1 to 6.
            target: How long a segment lasts, in milliseconds, 300 to 10000.
            port: The HTTP port.
            path: A folder to serve under as well, such as "/run7", which also
                gives `/run7/stream.m3u8`. A TV caches by URL, so give each
                run its own. Starts with "/", under 32 characters.

        Raises:
            ValueError: An argument is out of range.
            OSError: The H.264 encoder is busy, or `h264enc` or `tsmux` is
                not in the firmware.
            MemoryError: The buffers did not fit.
        """
    def start(self) -> None:
        """Start serving.

        Raises:
            RuntimeError: It is already running.
            OSError: The port can't be listened on, or the firmware has no
                network.
        """
    def offer(self, framebuffer: bytes | bytearray | memoryview) -> bool:
        """Give the task a finished frame to encode.

        It copies the frame into a back buffer, so the task never reads a
        frame you are still drawing. A frame offered while the task still
        holds the last one is skipped, not queued.

        Args:
            framebuffer: Native-order RGB565 pixels, at least
                `width * height * 2` bytes.

        Returns:
            True when the frame was taken, False when the task was busy.
        """
    def stats(self) -> dict[str, int | bool | str]:
        """Count what the task has done.

        Returns:
            A dict with `frames`, `offers`, `requests` and `overruns`, the
            PPA's and encoder's times `ppa_us` and `enc_us` in microseconds,
            `running`, and `player`, a description of the last client.
        """
    def stop(self) -> None:
        """Stop serving."""
    def close(self) -> None:
        """Stop serving and free the encoder and the buffers."""

class Cast:
    """Sends a framebuffer as RTP to a Miracast (Wi-Fi Display) receiver.

    Your code does the discovery and the RTSP exchange; once the receiver has
    answered PLAY, `start` begins sending and the session is idle in Python.
    The stream is `tsmux`'s: program 1, H.264 on PID 0x100, and, with
    `audio=True`, 48 kHz stereo LPCM.

    With audio, the track rides the video's clock. Feed it 10 ms blocks with
    `feed_audio`, or give it an audiodsp `audiopump.Tap` with `set_tap` and the
    task reads the pump itself. The task keeps the track on the wall clock by
    dropping or inserting one 10 ms block at a time, never by jumping.

    Only one H.264 encoder exists, so a second `Cast`, an `Hls` or an
    `h264enc.Encoder` raises `OSError` while another has it.
    """

    def __init__(
        self,
        width: int,
        height: int,
        *,
        canvas_w: int = 0,
        canvas_h: int = 0,
        fps: int = 30,
        gop: int = 30,
        bitrate: int = 3_000_000,
        qp_min: int = 10,
        qp_max: int = 45,
        out_size: int = 0,
        audio: bool = False,
    ) -> None:
        """Open the encoder. Nothing is sent until `start`.

        Args:
            width: Frame width, a multiple of 16, 80 to 1920.
            height: Frame height, a multiple of 16, 80 to 2032.
            canvas_w: With `canvas_h`, a larger black picture to centre the
                frame on. 0 means no canvas.
            canvas_h: See `canvas_w`.
            fps: Frames per second.
            gop: Frames from one IDR (key) frame to the next.
            bitrate: Target bits per second.
            qp_min: Lowest quantiser.
            qp_max: Highest quantiser.
            out_size: The most bytes one access unit may take, as for
                `h264enc.Encoder`. 0 sizes it for a keyframe of the canvas.
            audio: Carry a 48 kHz stereo LPCM track.

        Raises:
            ValueError: A size is out of range.
            OSError: The H.264 encoder is busy, or `h264enc` or `tsmux` is
                not in the firmware.
            MemoryError: A buffer did not fit.
        """
    def start(self, framebuffer: bytes | bytearray | memoryview, sink_ip: str, dst_port: int, src_port: int, /) -> None:
        """Begin sending, after the receiver has answered RTSP PLAY.

        Args:
            framebuffer: The RGB565 pixels to send, at least
                `width * height * 2` bytes. The task reads it as it runs, so
                keep it alive and draw into it.
            sink_ip: The receiver's address, such as "192.168.1.30".
            dst_port: The receiver's RTP port.
            src_port: The local port to send from.

        Raises:
            RuntimeError: It is already casting.
            ValueError: The framebuffer is too small.
            OSError: The socket can't be made or bound, or the firmware has
                no network.
        """
    def stop(self) -> None:
        """End the cast."""
    def force_idr(self) -> None:
        """Make the next frame an IDR frame, which a receiver joining late needs."""
    def set_bitrate(self, bps: int) -> None:
        """Change the target bitrate, from the next frame."""
    def set_fps(self, fps: int) -> None:
        """Change the frame rate, from the next frame. Values below 1 mean 1."""
    def set_skip(self, ms: int) -> None:
        """Set the longest a still picture may go without a frame.

        The cast skips frames that match the last one, but sends one at least
        this often. It starts at 500 ms.

        Args:
            ms: Milliseconds; 0 sends every frame.
        """
    def mark_dirty(self) -> None:
        """Say the picture changed, so the next frame is sent without waiting."""
    def feed_audio(self, block: bytes | bytearray | memoryview) -> bool:
        """Add one block of audio.

        Args:
            block: 10 ms of 48 kHz stereo 16-bit little-endian PCM, 1920
                bytes.

        Returns:
            True when it was taken. False when the ring is full (you are
            ahead of real time), when the cast was made without `audio`, or
            when a tap is feeding it.

        Raises:
            ValueError: The block is under 1920 bytes.
        """
    def set_sync(self, ms: int) -> None:
        """Set how far the audio may drift from the clock before it is corrected.

        Args:
            ms: Milliseconds; 0 measures the drift and never corrects it.
        """
    def set_tap(self, tap: object | None, rate: int = 48000, channels: int = 2) -> None:
        """Take the audio from an audiodsp `audiopump.Tap`, read by the task.

        Args:
            tap: The tap, or None to go back to `feed_audio`. Removing it
                needs the cast stopped.
            rate: The tap's sample rate, 24000 or 48000.
            channels: 1 or 2; it must match the tap.

        Raises:
            ValueError: The cast was made without `audio=True`, the firmware
                has no audiodsp tap reader, the rate or channels are not
                supported or the tap's channel count differs.
        """
    def stats(self) -> dict[str, int | bool]:
        """Count what the task has done.

        Returns:
            A dict. Video: `frames`, `fps` (times 1000), `packets`, `sent`,
            `stalls`, `idr`, `skipped`, `length` and `frame_type` of the last
            frame, and the times `enc_us`, `ppa_us` and `mux_us`. Audio:
            `audio_fed`, `audio_muxed`, `audio_underruns`, `audio_level`
            (blocks waiting), `audio_inserted`, `audio_dropped`,
            `audio_rejoins`, `audio_drift_ms` and `trim_ppm`. Tap: `tap_bytes`,
            `tap_lapped`, `tap_full` and `tap_hz`. And `stack_free`, the
            task's spare stack, and `running`.
        """
    def close(self) -> None:
        """Stop and free the encoder and the muxer."""
