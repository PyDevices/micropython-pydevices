# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""MPEG-TS and HLS from H.264.

`Segmenter` wraps H.264 access units in MPEG-TS and cuts them into HLS
segments with a playlist, so a board can serve live video to anything that
plays HLS: a Roku, VLC, a browser with hls.js. `Muxer` is the muxer underneath,
one call at a time.

Access units are Annex B, as `h264enc` returns them. Times are in 90 kHz
ticks. The stream is program 1 with the PMT on PID 0x1000, H.264 on 0x100
(which carries the PCR, 400 ms ahead of the PTS) and an access-unit delimiter
before each frame.

It is portable C and runs on every port.
"""

__revision__: str
"""Which source this firmware was built from, from `git describe`: a release
tag, or the nearest one and the commit. "unknown" when the build had no git."""

class Segmenter:
    """Cuts a video stream into HLS segments and keeps the latest few.

    A segment starts at a keyframe, ends at the first keyframe at least
    `target` milliseconds later, and begins with the PAT and PMT, so each plays
    on its own. Serve `playlist` as the stream's `.m3u8` and `segment` for
    each file it names.

    One-second segments and three kept give the least delay that still plays
    steadily on a Roku, about 7 to 10 seconds.

    `first` and `count` are read-only properties.
    """

    first: int
    """The number of the oldest segment still held."""
    count: int
    """How many finished segments are held."""

    def __init__(self, segments: int = 3, target: int = 1000) -> None:
        """Make a segmenter.

        Args:
            segments: How many finished segments to keep, 1 to 16.
            target: The shortest a segment may be, in milliseconds, 100 to
                60000.

        Raises:
            ValueError: An argument is out of range.
        """
    def add(self, au: bytes | bytearray | memoryview, pts: int, key: bool) -> bool:
        """Add one frame.

        Args:
            au: The access unit, Annex B.
            pts: Its presentation time in 90 kHz ticks.
            key: Whether it is a keyframe.

        Frames before the first keyframe are dropped.

        Returns:
            True when this frame finished a segment.
        """
    def playlist(self, prefix: str = "seg") -> str:
        """Make the HLS media playlist of the segments held.

        `#EXT-X-MEDIA-SEQUENCE` counts up as old segments go, so a player
        following it live sees a moving window.

        Args:
            prefix: What each segment's file name starts with; the file is
                the prefix, the segment's number and ".ts".
        """
    def segment(self, n: int) -> bytes | None:
        """Give one finished segment's bytes.

        Args:
            n: The segment's number, from `first` up to `first + count - 1`.

        Returns:
            The MPEG-TS bytes, or None once the segment has gone or before it
            exists.
        """

class Muxer:
    """Writes MPEG-TS packets, one call at a time.

    Each method returns the 188-byte packets it produced, joined as bytes.
    This is the stream `castif` sends to Miracast receivers. LPCM audio is
    carried as HDMV stream type 0x83 on PID 0x101, which is not an HLS audio
    format: a Roku plays it silent.
    """

    def __init__(self, *, lpcm: bool = False) -> None:
        """Make a muxer.

        Args:
            lpcm: True adds an LPCM audio stream, which `lpcm` then feeds.
        """
    def tables(self) -> bytes:
        """Return the PAT and PMT. Send them before each keyframe."""
    def video(self, au: bytes | bytearray | memoryview, pts: int, key: bool = False) -> bytes:
        """Wrap one H.264 access unit.

        Args:
            au: The access unit, Annex B.
            pts: Its presentation time in 90 kHz ticks.
            key: Whether it is a keyframe.
        """
    def lpcm(self, pcm: bytes | bytearray | memoryview, pts: int) -> bytes:
        """Wrap a block of audio.

        Args:
            pcm: 48 kHz stereo signed 16-bit samples in the chip's native
                byte order, whole stereo frames, under 65000 bytes.
            pts: The block's presentation time in 90 kHz ticks.

        Raises:
            ValueError: The muxer was made without `lpcm=True`, or the block
                is not whole stereo frames under 65000 bytes.
        """
    def pcr(self, pcr: int) -> bytes:
        """Write a clock reference alone, for a tick with no frame.

        Args:
            pcr: The reference, in 90 kHz ticks.
        """
    def reset(self) -> None:
        """Start the stream over, with the packet continuity counters back at zero."""
