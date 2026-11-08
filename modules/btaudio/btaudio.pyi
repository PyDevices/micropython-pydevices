# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Receive Bluetooth audio from a phone or computer, as a speaker does.

The board appears in the sender's Bluetooth list, pairs without a PIN, and
receives what it plays (A2DP). `Receiver.readinto` hands over the decoded
audio for any PCM output. Through AVRCP you read everything the sender says
about the track, cover art included, its play status and position, and you
drive its transport, volume and player settings. Through HFP you follow the
phone itself: its battery, signal, carrier and calls. Classic ESP32 only.

A call's audio comes to the board too. While it's up, `Receiver.readinto`
returns the phone's voice instead of music, at the call's rate, and
`Receiver.write` sends the board's microphone back.
"""

CONNECTION: int
"""`Receiver.changes` bit: a sender connected or left."""
STREAM: int
"""`Receiver.changes` bit: audio started or stopped flowing."""
FORMAT: int
"""`Receiver.changes` bit: the stream's sample rate is known or changed, or a
call's audio came or went."""
TRACK: int
"""`Receiver.changes` bit: the track's metadata changed: title, artist,
album, genre, number, count or duration."""
PLAYING: int
"""`Receiver.changes` bit: the play status changed."""
VOLUME: int
"""`Receiver.changes` bit: the volume changed, here or on the sender."""
POSITION: int
"""`Receiver.changes` bit: the sender reported where playback is."""
PEER: int
"""`Receiver.changes` bit: the sender's name, signal, battery or link details
changed."""
COVER: int
"""`Receiver.changes` bit: a new cover image arrived, or the track has none."""
PHONE: int
"""`Receiver.changes` bit: the phone's battery, signal, carrier or other
hands-free state changed."""
CALL: int
"""`Receiver.changes` bit: a call started, changed state or ended, or the
caller is known."""

KEY_VOLUME_UP: int
"""`Receiver.key` code for the sender's volume up key."""
KEY_VOLUME_DOWN: int
KEY_MUTE: int
KEY_REWIND: int
"""`Receiver.key` code for rewind: hold it with state 0, release with 1."""
KEY_FAST_FORWARD: int

class Receiver:
    """The board's Bluetooth audio receiver. There is one: a second
    `Receiver()` takes over the running one, link and all, which is how a
    program picks the receiver up again after a soft reset.

    A property the sender hasn't reported reads None until it does. Not
    every sender reports everything: a podcast app may send no album, genre
    or track number.

    Args:
        name: what senders list the board as. Defaults to the board's
            hostname, `network.hostname()`.
        buffer_ms: how much audio to hold, 50 to 5000. Playback starts once
            half of it has arrived, so that half is the delay to the
            speaker. The other half absorbs reads that come late: a program
            that redraws a screen between reads wants 1000.
        reconnect: call back the last sender, if it is still paired, as a
            speaker does when it's switched on.
    """

    connected: bool
    """A sender is connected."""
    streaming: bool
    """Audio is flowing from the sender."""
    playing: bool
    """The sender says it is playing."""
    call_audio: bool
    """A call's audio is up: `readinto` returns the phone's voice at
    `sample_rate` (8000, or 16000 for wideband), and `write` sends the
    microphone. A phone's video or voice-chat app opens it too."""
    status: str | None
    """The sender's play status: "stopped", "playing", "paused",
    "seeking forward", "seeking back" or "error"."""
    sample_rate: int
    """The stream's rate in Hz, or 0 before the first stream."""
    volume: int
    """0 to 127, shared with the sender's volume slider. Setting it moves the
    slider; the sender moving it changes this."""
    title: str
    """The current track's title, or "" if the sender hasn't said."""
    artist: str
    album: str
    genre: str
    track_number: int | None
    """The track's place on its album, from 1."""
    track_count: int | None
    """How many tracks the album has."""
    duration: int | None
    """The track's length in milliseconds."""
    position: int | None
    """Where playback is, in milliseconds. It moves on between the sender's
    reports while playing, so it can be read at any rate."""
    cover: bytes | None
    """The track's cover art as a JPEG, 200 by 200 pixels, or None while it
    is on its way or when the sender has none. Each read copies the bytes,
    so read it once after each `COVER` change."""
    peer: str
    """The sender's Bluetooth address, or "" with none connected."""
    peer_name: str
    """The sender's device name, or "" before it is known."""

    def __init__(self, name: str | None = None, *, buffer_ms: int = 500, reconnect: bool = True) -> None: ...
    def readinto(self, buf: bytearray | memoryview) -> int:
        """Fill `buf` with 16-bit stereo frames at `sample_rate`, scaled by
        `volume`. Never waits: silence fills whatever the stream can't.

        Returns:
            How many frames came from the stream.
        """
    def write(self, buf: bytes | bytearray | memoryview) -> int:
        """Send microphone audio to the phone during a call: 16-bit mono at
        `sample_rate`. Up to half a second queues; it goes out once 120 ms
        has, and silence goes out until then.

        Returns:
            How many bytes were taken: fewer when the queue is full, 0 with
            no call audio up.
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
    def stop(self) -> None: ...
    def next(self) -> None: ...
    def previous(self) -> None: ...
    def seek(self, direction: int) -> None:
        """Hold fast forward (1) or rewind (-1) on the sender until the next
        `seek(0)`. Starting one direction releases the other."""
    def key(self, code: int, state: int = 2) -> None:
        """Send any AVRCP pass-through key, such as `KEY_MUTE`.

        Args:
            code: the key's AVRCP operation id, 0 to 127.
            state: 0 presses the key, 1 releases it, 2 does both.
        """
    def setting(self, name: str, value: str) -> None:
        """Ask the sender to change a player setting. Many senders ignore
        these, and AVRCP has no way to read them back.

        Args:
            name: "equalizer", "repeat", "shuffle" or "scan".
            value: "off" or "on" for the equalizer; "off", "single" or "all"
                for repeat; "off", "all" or "group" for shuffle and scan.

        Raises:
            ValueError: an unknown name, or a value that name doesn't take.
        """
    def refresh(self) -> None:
        """Ask the sender for its play status, position and signal now,
        instead of at the next one-second poll."""
    def link(self) -> dict:
        """The connection, as a dict.

        Returns:
            address and name: the sender's, as text. rssi_delta: the signal
            in dB from the controller's ideal range, 0 inside it, None
            unknown. codec ("SBC"), sample_rate, channels, channel_mode
            ("mono", "dual", "stereo" or "joint stereo"), block_length,
            subbands, allocation ("snr" or "loudness"), bitpool_min and
            bitpool_max: the stream as configured. delay_ms: the delay
            reported to the sender, which it may use to move video to match.
            avrcp_features and avrcp_target_flags: the sender's AVRCP
            feature bits. cover_art: the cover art channel is open.
            battery: the sender's battery as AVRCP reports it ("normal",
            "warning", "critical", "external" or "full"), or None.
            hands_free: the HFP link is up.
        """
    def phone(self) -> dict:
        """The phone's state over the hands-free link, as a dict. A computer
        has no hands-free link, and then connected is False.

        Returns:
            connected: the link is up. service and roaming: bools. signal and
            battery: 0 to 5. operator: the network's name. number: the
            phone's own number, when it says. call: "idle", "incoming",
            "dialing", "alerting", "active" or "held". caller: the number of
            the current or last call. held: 0 none, 1 one held beside an
            active one, 2 held only. waiting: a second call is waiting.
            ringing: the phone is ringing now. voice: voice recognition is on.
            in_band_ring: the phone sends its own ringtone. speaker_gain and
            mic_gain: 0 to 15.
        """
    def answer(self) -> None:
        """Answer the incoming call."""
    def hangup(self) -> None:
        """End the active call, or reject the incoming one."""
    def dial(self, number: str | None = None) -> None:
        """Have the phone call `number`, or redial the last one."""
    def voice(self, on: bool) -> None:
        """Start or stop the phone's voice assistant."""
    def dtmf(self, digits: str) -> None:
        """Send touch tones during a call, one per character: 0 to 9, * and #."""
    def disconnect(self) -> None:
        """Drop the sender. The board is discoverable again afterwards."""
    def close(self) -> None:
        """Shut Bluetooth down. A new `Receiver()` starts it again."""
    def __enter__(self) -> Receiver: ...
    def __exit__(self, *args: object) -> None: ...
