# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Receive Bluetooth audio from a phone or computer, as a speaker does.

The board appears in the sender's Bluetooth list, pairs without a PIN, and
receives what it plays (A2DP). `Receiver.readinto` hands over the decoded
audio for any PCM output, and the sender's title, play state and volume are
read and its transport driven through AVRCP. Classic ESP32 only.
"""

CONNECTION: int
"""`Receiver.changes` bit: a sender connected or left."""
STREAM: int
"""`Receiver.changes` bit: audio started or stopped flowing."""
FORMAT: int
"""`Receiver.changes` bit: the stream's sample rate is known or changed."""
TRACK: int
"""`Receiver.changes` bit: the title, artist or album changed."""
PLAYING: int
"""`Receiver.changes` bit: the sender started or stopped playing."""
VOLUME: int
"""`Receiver.changes` bit: the volume changed, here or on the sender."""

class Receiver:
    """The board's Bluetooth audio receiver. There is one: a second
    `Receiver()` takes over the running one, link and all, which is how a
    program picks the receiver up again after a soft reset.

    Args:
        name: what senders list the board as. Defaults to the board's
            hostname, `network.hostname()`.
        buffer_ms: how much audio to hold, 50 to 5000. Playback starts once
            half of it has arrived, so this is also the delay to the speaker.
        reconnect: call back the last sender, if it is still paired, as a
            speaker does when it's switched on.
    """

    connected: bool
    """A sender is connected."""
    streaming: bool
    """Audio is flowing from the sender."""
    playing: bool
    """The sender says it is playing."""
    sample_rate: int
    """The stream's rate in Hz, or 0 before the first stream."""
    volume: int
    """0 to 127, shared with the sender's volume slider. Setting it moves the
    slider; the sender moving it changes this."""
    title: str
    """The current track's title, or "" if the sender hasn't said."""
    artist: str
    album: str
    peer: str
    """The sender's Bluetooth address, or "" with none connected."""

    def __init__(self, name: str | None = None, *, buffer_ms: int = 500, reconnect: bool = True) -> None: ...
    def readinto(self, buf: bytearray | memoryview) -> int:
        """Fill `buf` with 16-bit stereo frames at `sample_rate`, scaled by
        `volume`. Never waits: silence fills whatever the stream can't.

        Returns:
            How many frames came from the stream.
        """
    def changes(self) -> int:
        """The bits (`CONNECTION`, `TRACK` ...) for what changed since the
        last call, which clears them."""
    def levels(self) -> tuple[int, int]:
        """The loudest left and right samples since the last call, 0 to
        32767, before `volume`."""
    def stats(self) -> dict[str, int]:
        """Counters for the buffer: buffered, capacity, received,
        underruns, overruns, dropped and repeated frames."""
    def play(self) -> None: ...
    def pause(self) -> None: ...
    def next(self) -> None: ...
    def previous(self) -> None: ...
    def disconnect(self) -> None:
        """Drop the sender. The board is discoverable again afterwards."""
    def close(self) -> None:
        """Shut Bluetooth down. A new `Receiver()` starts it again."""
    def __enter__(self) -> Receiver: ...
    def __exit__(self, *args: object) -> None: ...
