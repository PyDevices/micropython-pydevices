# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Bluetooth Classic: the radio, and the profiles that make a board a speaker
or a hands-free unit.

`Bluetooth` is the radio, one per board, shaped like `bluetooth.BLE`: turn
it on with `active`, and read and set its name, visibility and pairing PIN
with `config`. Each profile is an object of its own that takes the radio:

- `AudioSink` receives audio from a phone or computer (A2DP sink) and talks
  to its player (AVRCP controller): what's playing, cover art included, its
  position and transport, and the volume slider shared with the sender.
- `HandsFree` follows the phone itself (HFP hands-free unit): battery,
  signal, carrier and calls, which it can answer, end and dial, with the
  call's audio both ways.
- `AudioSource` sends audio to headphones, a speaker or a car stereo (A2DP
  source), takes their play, pause, skip and volume keys, and tells them the
  title and artist to show (AVRCP target).
- `Serial` is a serial port over Bluetooth (SPP), a stream like
  `machine.UART`, for a PC's Bluetooth COM port, a phone's terminal app or
  another board.
- `HIDDevice` makes the board a keyboard, mouse or gamepad for a PC or
  phone, and `HIDHost` reads one: keys, moves and buttons, as reports.

A board is a sink or a source, one at a time: `deinit()` the one before
making the other.

Audio is a stream: `AudioSink.readinto` and `HandsFree.readinto` fill a
buffer with 16-bit PCM and never wait, and `HandsFree.write` takes the
microphone. Your program chooses what to play: the music, or the call while
`HandsFree.audio` is True.

State is properties. Nothing calls back into Python from the Bluetooth task:
each object's `changes()` returns a bit for each kind of state that changed
since the last call, so a program polls it as often as it redraws. The bits
are module constants, and each object returns only its own.

Everything keeps running across a soft reset. Making an object again takes
over the running one, link and all, which is how a program picks Bluetooth
up again after Ctrl-D.

Addresses are 6 bytes, most significant first, as `bluetooth.BLE` gives
them.
"""

# -- changes() bits ---------------------------------------------------------

CONNECTION: int
"""`AudioSink.changes`, `AudioSource.changes`, `HandsFree.changes`,
`Serial.changes`, `HIDDevice.changes` and `HIDHost.changes` bit: the link
came up or went down."""
STREAM: int
"""`AudioSink.changes` bit: audio started or stopped flowing."""
FORMAT: int
"""`AudioSink.changes` bit: the stream's sample rate is known or changed."""
TRACK: int
"""`AudioSink.changes` bit: the track's metadata changed: title, artist,
album, genre, number, count or duration."""
PLAYING: int
"""`AudioSink.changes` bit: the play status changed."""
VOLUME: int
"""`AudioSink.changes` bit: the volume changed, here or on the sender."""
POSITION: int
"""`AudioSink.changes` bit: the sender reported where playback is."""
PEER: int
"""`AudioSink.changes` bit: the sender's name, signal, battery or link
details changed."""
COVER: int
"""`AudioSink.changes` bit: a new cover image arrived, or the track has
none."""
PHONE: int
"""`HandsFree.changes` bit: the phone's battery, signal, carrier, gains or
other hands-free state changed."""
CALL: int
"""`HandsFree.changes` bit: a call started, changed state or ended, or the
caller is known."""
AUDIO: int
"""`HandsFree.changes` bit: the call's audio came or went. `sample_rate`
says its rate while it's up."""
PAIRED: int
"""`Bluetooth.changes` bit: a device paired, or a pairing was removed."""
FOUND: int
"""`Bluetooth.changes` bit: an inquiry found a device, or finished."""
SERIAL: int
"""`Serial.changes` bit: bytes arrived."""
REMOTE: int
"""`AudioSource.changes` bit: the receiver pressed a key; read it with
`AudioSource.key`."""
HID: int
"""`HIDDevice.changes` and `HIDHost.changes` bit: a report came, the host
set an output report (a keyboard's LEDs), the protocol changed, or there's
room to send again."""

HID_KEYBOARD: int
"""`HIDDevice` subclass: a keyboard."""
HID_MOUSE: int
"""`HIDDevice` subclass: a mouse or other pointer."""
HID_GAMEPAD: int
"""`HIDDevice` subclass: a gamepad."""

# -- AVRCP key codes -----------------------------------------------------------

KEY_PLAY: int
"""Key code for play: `AudioSink.key` sends it, `AudioSource.key` reports
it."""
KEY_STOP: int
KEY_PAUSE: int
KEY_NEXT: int
KEY_PREVIOUS: int
KEY_VOLUME_UP: int
"""`AudioSink.key` code for the sender's volume up key."""
KEY_VOLUME_DOWN: int
KEY_MUTE: int
KEY_REWIND: int
"""`AudioSink.key` code for rewind: hold it with state 0, release with 1."""
KEY_FAST_FORWARD: int

class Bluetooth:
    """The board's Bluetooth Classic radio. There is one: every
    `Bluetooth()` returns it.

    The radio starts off. Turn it on with `active(True)`, or let the first
    profile do it: a program reads as `bt = Bluetooth()`, then
    `AudioSink(bt)`.

    Pairing needs nothing typed on the board: a device that asks to compare
    numbers is accepted, and an older device that asks for a PIN gets the
    `pin` setting.
    """

    def __init__(self) -> None: ...
    def active(self, active: bool | None = None) -> bool:
        """Turn the radio on or off, and say whether it's on.

        Turning it off ends every profile's links and shuts Bluetooth down.
        The methods of profile objects made before then raise OSError, and
        their properties read as not connected, until the profile is made
        again.

        Args:
            active: True starts the radio, False stops it, None only asks.

        Returns:
            Whether the radio is on.
        """
    def config(self, *names: str, **settings: object) -> object:
        """Read one setting by name, or change settings by keyword.

        `config("mac")` reads, `config(name="kitchen")` sets. Settings:

        - name (str): what other devices list the board as. Defaults to the
          board's hostname, `network.hostname()`.
        - discoverable (bool): other devices can find the board when they
          search. On by default. A profile that takes one device at a time
          hides the board while that device is connected, and shows it
          again afterwards if this is True.
        - connectable (bool): devices already paired can connect. On by
          default.
        - pin (str): the PIN for devices that pair the old way, 1 to 16
          characters, "0000" by default as on most speakers.
        - device_class (int): the Class of Device, the 24 bits a computer
          or phone uses to pick an icon and decide what to offer. By default
          the profiles running set it: Audio/Video while a sink, source or
          hands-free runs (with a keyboard too), Peripheral for a keyboard,
          mouse or game controller alone, plus the Audio, Rendering,
          Capturing and Telephony service bits those profiles give. Set an
          int to use your own whatever runs, for example
          `config(device_class=0x240404)` for a headset, and None to go back
          to the default. Reading it gives the class in use.
        - mac (bytes): the board's Bluetooth address. Read only, and only
          while the radio is on.

        Raises:
            ValueError: an unknown setting, or a value it doesn't take.
        """
    def paired(self) -> list[bytes]:
        """The addresses of the devices the board is paired with. Pairings
        are kept in flash and survive a reset."""
    def unpair(self, addr: bytes | None = None) -> None:
        """Forget a paired device, or every one with no argument. The
        device has to pair again before it can connect."""
    def changes(self) -> int:
        """The bits (`PAIRED`, `FOUND`) for what changed since the last
        call, which clears them."""
    def inquiry(self, duration_ms: int = 10240) -> None:
        """Look for devices that can be found, for headphones or a speaker
        to send to. Results so far are forgotten; `found` lists the new ones
        as they answer, and `FOUND` is set for each and when it ends.

        Args:
            duration_ms: how long to look, 1280 to 61440, in steps of 1280.
                0 stops an inquiry that's running.

        Raises:
            OSError: the radio is off.
        """
    def inquiring(self) -> bool:
        """An inquiry is running."""
    def found(self) -> list[tuple[bytes, str, int, int | None]]:
        """What the last inquiry found, up to 20 devices: (address, name,
        class of device, RSSI in dBm). The name is "" when the device didn't
        send one with its answer, and the RSSI None when the controller
        didn't measure it. Class of device bits 8 to 12 are the major class:
        4 is audio and video, which headphones, speakers and car stereos
        report."""

class AudioSink:
    """Receive audio from a phone or computer, as a speaker does, and drive
    its player.

    The board accepts one sender at a time, and pairs without a PIN. Through
    A2DP the sender's audio arrives and `readinto` hands it over decoded,
    for any PCM output. Through AVRCP you read everything the sender says
    about the track, cover art included, its play status and position, and
    you drive its transport, volume and player settings.

    There is one sink. A second `AudioSink` takes over the running one, link
    and all, and its arguments then change nothing.

    A property the sender hasn't reported reads None until it does. Not
    every sender reports everything: a podcast app may send no album, genre
    or track number.

    Args:
        bt: the radio, `Bluetooth()`. It is turned on if it's off.
        buffer_ms: how much audio to hold, 50 to 5000. Playback starts once
            half of it has arrived, so that half is the delay to the
            speaker. The other half absorbs reads that come late: a program
            that redraws a screen between reads wants 1000. The default
            depends on the board: 500 where there is PSRAM, 250 otherwise.
        reconnect: call the last sender back, if it is still paired, as a
            speaker does when it's switched on.
    """

    connected: bool
    """A sender is connected."""
    streaming: bool
    """Audio is flowing from the sender."""
    playing: bool
    """The sender says it is playing."""
    status: str | None
    """The sender's play status: "stopped", "playing", "paused",
    "seeking forward", "seeking back" or "error"."""
    sample_rate: int
    """The stream's rate in Hz, or 0 before the first stream. Phones send
    44100 almost always; A2DP allows 16000, 32000 and 48000 too."""
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
    cover_size: int
    """The cover's length in bytes, 0 with none."""
    peer: bytes | None
    """The sender's address, or None with none connected."""
    peer_name: str
    """The sender's device name, or "" before it is known."""

    def __init__(self, bt: Bluetooth, *, buffer_ms: int | None = None, reconnect: bool = True) -> None: ...
    def readinto(self, buf: bytearray | memoryview) -> int:
        """Fill `buf` with 16-bit stereo frames (4 bytes each) at
        `sample_rate`, scaled by `volume`. Never waits: silence fills
        whatever the stream can't.

        Read a block when your output has room for one, so its clock sets
        the pace. The sender's clock and your output's never quite agree, so
        the sink keeps its buffer near half full by dropping or repeating
        one frame now and then.

        Returns:
            How many frames came from the stream.

        Raises:
            ValueError: `buf` doesn't hold whole frames.
        """
    def cover_into(self, buf: bytearray | memoryview) -> int:
        """Copy the cover's JPEG into `buf`, a buffer you keep, so each new
        cover makes no garbage. Thumbnails are a few tens of KB.

        Returns:
            Its length in bytes, or 0 with no cover.

        Raises:
            ValueError: `buf` is smaller than the cover (`cover_size`).
        """
    def changes(self) -> int:
        """The bits (`CONNECTION`, `TRACK` ...) for what changed since the
        last call, which clears them."""
    def levels(self) -> tuple[int, int]:
        """The loudest left and right samples since the last call, 0 to
        32767, before `volume`."""
    def stats(self) -> dict[str, int]:
        """Counters for the buffer, in frames: buffered, capacity,
        received, underruns, overruns, dropped and repeated (one at a time,
        to follow the sender's clock), and skipped (the oldest, when a read
        that came late finds the buffer over its fill point as playback
        starts)."""
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
            address (bytes or None) and name: the sender's. rssi_delta: the
            signal in dB from the controller's ideal range, 0 inside it,
            None unknown. codec ("SBC"), sample_rate, channels, channel_mode
            ("mono", "dual", "stereo" or "joint stereo"), block_length,
            subbands, allocation ("snr" or "loudness"), bitpool_min and
            bitpool_max: the stream as configured. delay_ms: the delay
            reported to the sender, which it may use to move video to match.
            avrcp_features and avrcp_target_flags: the sender's AVRCP
            feature bits. cover_art: the cover art channel is open.
            battery: the sender's battery as AVRCP reports it ("normal",
            "warning", "critical", "external" or "full"), or None.
        """
    def connect(self, addr: bytes | None = None) -> None:
        """Call a paired sender, or the last one with no argument. The link
        comes up later; watch for `CONNECTION`."""
    def disconnect(self) -> None:
        """Drop the sender. The board is discoverable again afterwards."""
    def deinit(self) -> None:
        """Drop the sender and stop the sink. The radio stays on, and a new
        `AudioSink` starts the sink again. Until then the methods raise
        OSError and the properties read as not connected."""
    def __enter__(self) -> AudioSink: ...
    def __exit__(self, *args: object) -> None:
        """`deinit`."""

class HandsFree:
    """Follow a phone as a car kit or a headset does: the phone's battery,
    signal and carrier, its calls, and their audio.

    When `AudioSink` connects to a phone, the hands-free link follows it to
    the same phone. Without a sink, `connect` calls a phone yourself. A
    computer has no hands-free side, and then `connected` stays False.

    During a call's audio, `readinto` gives you the phone's voice and
    `write` takes your microphone. The phone decides when the audio comes
    to the board: for a call when the board is its audio device, and for a
    voice or video chat app. `connect_audio` asks for it and
    `disconnect_audio` hands it back to the phone.

    There is one hands-free unit. A second `HandsFree` takes over the
    running one, link and all, and its arguments then change nothing.

    Args:
        bt: the radio, `Bluetooth()`. It is turned on if it's off.
        buffer_ms: how much of the phone's voice to hold, 50 to 1000.
            Playback starts once half of it has arrived, so that half is the
            delay in the conversation.
        channels: 1 to read the call as it comes, mono; 2 to read it copied
            to both sides, for an output that is always stereo.
    """

    connected: bool
    """The hands-free link to the phone is up."""
    peer: bytes | None
    """The phone's address, or None with no link."""
    service: bool | None
    """The phone has network service."""
    roaming: bool | None
    signal: int | None
    """The phone's signal, 0 to 5."""
    battery: int | None
    """The phone's battery, 0 to 5."""
    operator: str
    """The network's name, or "" before the phone says."""
    number: str
    """The phone's own number, or "" if it doesn't say."""
    call: str | None
    """The call's state: "idle", "incoming", "dialing", "alerting",
    "active" or "held"; None with no link."""
    caller: str
    """The number of the current or last call, or "" if unknown."""
    held: int
    """0 with no held call, 1 with a call held beside an active one, 2 with
    held calls only."""
    waiting: bool
    """A second call is waiting."""
    ringing: bool
    """The phone is ringing now."""
    in_band_ring: bool
    """The phone sends its own ringtone as call audio."""
    voice_recognition: bool
    """The phone's voice assistant is listening. Set it to start or stop the
    assistant; the phone may refuse, and then it stays as it was."""
    speaker_gain: int | None
    """The call's volume, 0 to 15, shared with the phone's call volume.
    `readinto` scales the call by it. Setting it tells the phone."""
    mic_gain: int | None
    """The microphone's gain the phone asks for, 0 to 15. Setting it tells
    the phone; the board doesn't apply it to what you `write`."""
    audio: bool
    """The call's audio is up: `readinto` returns the phone's voice and
    `write` sends the microphone."""
    sample_rate: int
    """The call audio's rate: 8000, or 16000 for wideband; 0 while it's not
    up."""

    def __init__(self, bt: Bluetooth, *, buffer_ms: int = 400, channels: int = 1) -> None: ...
    def readinto(self, buf: bytearray | memoryview) -> int:
        """Fill `buf` with the phone's voice: 16-bit frames of `channels`
        at `sample_rate`, scaled by `speaker_gain`. Never waits: silence
        fills whatever the call can't, and all of it while `audio` is False.

        Returns:
            How many frames came from the call.

        Raises:
            ValueError: `buf` doesn't hold whole frames.
        """
    def write(self, buf: bytes | bytearray | memoryview) -> int:
        """Send microphone audio to the phone: 16-bit mono at
        `sample_rate`. Up to half a second queues; it goes out once 120 ms
        has, and silence goes out until then.

        Returns:
            How many bytes were taken: fewer when the queue is full, 0 while
            `audio` is False.
        """
    def changes(self) -> int:
        """The bits (`CONNECTION`, `PHONE`, `CALL`, `AUDIO`) for what changed
        since the last call, which clears them."""
    def stats(self) -> dict[str, int]:
        """Counters for the call's buffer, as `AudioSink.stats` has them."""
    def answer(self) -> None:
        """Answer the incoming call."""
    def hangup(self) -> None:
        """End the active call, or reject the incoming one."""
    def dial(self, number: str | None = None) -> None:
        """Have the phone call `number`, or redial the last one."""
    def dtmf(self, digits: str) -> None:
        """Send touch tones during a call, one per character: 0 to 9, * and
        #."""
    def connect_audio(self) -> None:
        """Ask the phone to bring the call's audio to the board. Watch for
        `AUDIO`."""
    def disconnect_audio(self) -> None:
        """Hand the call's audio back to the phone; the call goes on."""
    def connect(self, addr: bytes | None = None) -> None:
        """Call a paired phone, or the sink's sender with no argument. The
        link comes up later; watch for `CONNECTION`."""
    def disconnect(self) -> None:
        """Drop the hands-free link. Music, if any, goes on."""
    def deinit(self) -> None:
        """Drop the link and stop the hands-free unit. The radio stays on,
        and a new `HandsFree` starts it again."""
    def __enter__(self) -> HandsFree: ...
    def __exit__(self, *args: object) -> None:
        """`deinit`."""

class AudioSource:
    """Send audio to headphones, a speaker or a car stereo, as a phone does.

    Through A2DP your program's PCM goes to the receiver, coded as SBC by
    the Bluetooth stack. Through AVRCP the receiver's play, pause, skip and
    volume keys come back to your program, and its display gets the title,
    artist and album you set, the play status, and the position.

    `write` takes 16-bit PCM at `rate` and `channels`, which you choose.
    The stream itself runs at `sample_rate`, 44100 Hz stereo on ESP-IDF 5,
    and the source converts between them, so a 16 kHz mono tone plays as
    it is. Writing as fast as you can is fine: while the stream runs,
    `write` waits for room, and the receiver's pace sets yours.

    There is one source, and a board runs a source or an `AudioSink`, not
    both: making one while the other runs raises OSError. A second
    `AudioSource` takes over the running one.

    SBC is the only codec. AAC needs ESP-IDF 6.1, and isn't offered yet.

    Args:
        bt: the radio, `Bluetooth()`. It is turned on if it's off.
        rate: the rate of what you `write`, 8000 to 48000 Hz.
        channels: what you `write`, 1 or 2.
        buffer_ms: how much audio to hold ahead of the stream, 50 to 5000.
            More rides out longer gaps between writes; the receiver hears
            it later. The default depends on the board: 500 where there is
            PSRAM, 250 otherwise.
    """

    connected: bool
    """A receiver is connected."""
    streaming: bool
    """Audio is flowing to the receiver."""
    sample_rate: int
    """The stream's rate in Hz, or 0 before it is agreed."""
    rate: int
    """The rate of what you `write`. Settable."""
    channels: int
    """1 or 2: what you `write`. Settable."""
    volume: int
    """0 to 127. With a receiver that takes absolute volume, which most
    headphones and car stereos do, this moves its own volume and follows
    its buttons; with one that doesn't, the source scales the audio.
    Settable."""
    status: str | None
    """The play status the receiver shows: "stopped", "playing" or
    "paused". Set it when your program starts or stops playing."""
    position: int | None
    """Where playback is in the track, in milliseconds. It counts on by
    itself while `status` is "playing". Setting it moves it, after a seek
    say."""
    title: str
    """The title set with `track`, or ""."""
    artist: str
    peer: bytes | None
    """The receiver's address, or None with none connected."""
    peer_name: str
    """The receiver's device name, or "" before it is known."""

    def __init__(self, bt: Bluetooth, *, rate: int = 44100, channels: int = 2, buffer_ms: int | None = None) -> None: ...
    def write(self, buf: bytes | bytearray | memoryview) -> int:
        """Send 16-bit PCM at `rate` and `channels`.

        While the stream runs, waits until all of `buf` is taken. Otherwise
        takes what fits and returns at once, so audio written before the
        stream starts plays first when it does.

        Returns:
            How many bytes were taken.
        """
    def changes(self) -> int:
        """The bits (`CONNECTION`, `STREAM`, `FORMAT`, `VOLUME`, `PEER`,
        `REMOTE`) for what changed since the last call, which clears
        them."""
    def key(self) -> tuple[int, bool] | None:
        """The receiver's next key, oldest first: (code, pressed), with
        codes such as `KEY_PLAY`, `KEY_PAUSE` and `KEY_NEXT`, pressed True
        when it went down and False when it came up. None when there are
        none. The source holds the last 16."""
    def track(self, title: str | None = None, artist: str | None = None, album: str | None = None,
              genre: str | None = None, *, number: int | None = None, count: int | None = None,
              duration: int | None = None) -> None:
        """Tell the receiver what's playing. A new title is a new track: the
        receiver hears of it, and `position` goes back to 0. None leaves a
        field as it is.

        Args:
            number: the track's place on its album, from 1.
            count: how many tracks the album has.
            duration: the track's length in milliseconds.
        """
    def stats(self) -> dict[str, int]:
        """Counters for the buffer, in frames: buffered, capacity,
        received (written), underruns (the stream found it empty and sent
        silence), overruns, dropped and repeated."""
    def link(self) -> dict:
        """The connection, as a dict.

        Returns:
            address (bytes or None) and name: the receiver's. codec
            ("SBC"), sample_rate, channels, channel_mode, bitpool_min and
            bitpool_max: the stream as agreed. delay_ms: the delay the
            receiver reported, for moving video to match. avrcp: the remote
            control is connected. absolute_volume: the receiver takes
            `volume` itself. metadata: this firmware can send the track to
            the receiver's display.
        """
    def connect(self, addr: bytes | None = None) -> None:
        """Connect to a receiver, or the last one with no argument. Pairing
        happens then if it's needed. The link comes up later; watch for
        `CONNECTION`. The stream starts by itself once it does."""
    def disconnect(self) -> None: ...
    def start(self) -> None:
        """Start the stream again after `suspend`."""
    def suspend(self) -> None:
        """Pause the stream, keeping the link: the receiver may go quiet or
        let another device play."""
    def deinit(self) -> None:
        """Drop the receiver and stop the source. The radio stays on, and an
        `AudioSink` can start now."""
    def __enter__(self) -> AudioSource: ...
    def __exit__(self, *args: object) -> None:
        """`deinit`."""

class Serial:
    """A serial port over Bluetooth (SPP), as a stream: `read`, `readinto`,
    `readline` and `write` work as on `machine.UART`, and `select.poll`
    takes it.

    Making it serves a port other devices can connect to: a computer pairs
    with the board and gets a Bluetooth COM port, or a phone's serial
    terminal app connects. `connect` calls another device's port instead.
    One link at a time.

    There is one serial port. A second `Serial` takes over the running one,
    link and all; its `name` and `rxbuf` then change nothing.

    Args:
        bt: the radio, `Bluetooth()`. It is turned on if it's off.
        name: the service name other devices list, "Serial Port" by
            default.
        rxbuf: bytes held for `read`, 256 to 65536. Bytes that arrive with
            it full are dropped and counted in `stats`.
        timeout: how long `read` waits for its first byte, in ms.
    """

    connected: bool
    """A device is connected."""
    listening: bool
    """The port is up for others to connect to."""
    peer: bytes | None
    """The connected device's address, or None."""
    channel: int | None
    """The RFCOMM channel the port listens on, which a device connecting
    by channel needs, or None before it's up."""
    timeout: int
    """How long `read` waits for its first byte, in ms. Settable."""

    def __init__(self, bt: Bluetooth, *, name: str = "Serial Port", rxbuf: int = 4096, timeout: int = 0) -> None: ...
    def read(self, nbytes: int = -1) -> bytes | None:
        """What has arrived, up to `nbytes`, or None when nothing came
        within `timeout`."""
    def readinto(self, buf: bytearray | memoryview, nbytes: int = -1) -> int | None: ...
    def readline(self) -> bytes | None: ...
    def write(self, buf: bytes | bytearray | memoryview) -> int:
        """Queue `buf` to send. Waits while the link is up until all of it is
        queued, so the link's pace sets yours.

        Raises:
            OSError: no device is connected.
        """
    def any(self) -> int:
        """How many bytes are waiting to be read."""
    def changes(self) -> int:
        """The bits (`CONNECTION`, `SERIAL`) for what changed since the last
        call, which clears them."""
    def connect(self, addr: bytes, channel: int = 0) -> None:
        """Call another device's serial port. With channel 0 the port is
        looked up on the device first. The link comes up later; watch for
        `CONNECTION`."""
    def disconnect(self) -> None: ...
    def stats(self) -> dict[str, int]:
        """Counters: rx_bytes, tx_bytes, and rx_dropped (bytes that found
        the receive buffer full)."""
    def deinit(self) -> None:
        """Drop the link and stop serving the port. The radio stays on."""
    def __enter__(self) -> Serial: ...
    def __exit__(self, *args: object) -> None:
        """`deinit`."""


class HIDDevice:
    """The board as a Bluetooth keyboard, mouse or gamepad.

    A PC or phone pairs with the board and reads the reports you send. The
    four descriptors here cover the usual devices, and the helpers build
    their reports for you; any other HID report descriptor works with
    `send`. One HID role runs at a time: deinit an `HIDHost` first.

    ```python
    kb = btclassic.HIDDevice(bt)                # a keyboard
    # pair it from the PC, then:
    kb.type("Hello from the board\n")
    ```

    The descriptors put each kind of report under a fixed report ID, the
    keyboard and mouse in the boot protocol's layouts, so a PC's firmware
    setup can use them too:

    - ID 1, keyboard: modifiers, 0, six key codes; output: the LEDs.
    - ID 2, mouse: buttons, x, y, wheel.
    - ID 3, consumer: one 16-bit media key usage.
    - ID 4, gamepad: 16 buttons, hat, x, y, z, rz.

    Args:
        bt: the radio, `Bluetooth()`. It is turned on if it's off.
        descriptor: the HID report descriptor: `KEYBOARD` (the default,
            with media keys), `MOUSE`, `COMBO` (keyboard, mouse and media
            keys), `GAMEPAD`, or your own bytes.
        subclass: what the host sees, `HID_KEYBOARD`, `HID_MOUSE`,
            `HID_GAMEPAD`, or `HID_KEYBOARD | HID_MOUSE` for both. None
            works it out from the descriptor.
        name: the service's name, shown by some hosts. None names it after
            the kind of device.
        boot: the descriptor's keyboard and mouse reports follow the boot
            protocol's layouts. None: True for this class's descriptors
            that have them, False for your own.
    """
    KEYBOARD: bytes
    """A keyboard with media keys: report IDs 1 and 3."""
    MOUSE: bytes
    """A five-button mouse with a wheel: report ID 2."""
    COMBO: bytes
    """A keyboard, mouse and media keys in one device: IDs 1, 2 and 3."""
    GAMEPAD: bytes
    """A gamepad with 16 buttons, a hat and four axes: report ID 4."""
    def __init__(self, bt: Bluetooth, descriptor: bytes = ..., *, subclass: int | None = None,
                 name: str | None = None, boot: bool | None = None) -> None: ...
    connected: bool
    """A host is connected."""
    peer: bytes | None
    """The host's address while connected."""
    boot: bool
    """The host chose the boot protocol."""
    output: bytes
    """The last output report the host set, report ID first when the
    descriptor has IDs."""
    leds: int
    """The keyboard's LEDs as the host set them: bit 0 Num Lock, 1 Caps
    Lock, 2 Scroll Lock. 0 without a keyboard report."""
    def connect(self, addr: bytes | None = None) -> None:
        """Call a host that has paired with the board, or with no address
        the last one it was connected to, remembered across resets (OSError
        if there is none). A paired host can also connect by itself; watch
        for `CONNECTION`."""
    def disconnect(self) -> None: ...
    def send(self, report: bytes) -> bool:
        """Queue an input report, report ID first when the descriptor has
        IDs, at most 64 bytes. Never waits: returns False when the queue
        is full, so wait for `HID` and send again, and False with no host
        connected, so nothing is typed into a window long after."""
    def keyboard(self, keys: tuple[int, ...] | list[int] = (), modifiers: int = 0) -> bool:
        """Hold these keys now, up to six HID key codes (4 is A, 0x28 Enter,
        0x4f to 0x52 the arrows), with these modifiers (bit 0 left Ctrl, 1
        left Shift, 2 left Alt, 3 left GUI, 4 to 7 the right ones).
        `keyboard()` releases everything. Waits for room while a host is
        connected; returns False with no host connected. mouse(),
        gamepad() and consumer() do the same."""
    def type(self, text: str, *, delay: int = 30) -> int:
        """Type text on a US layout: each character pressed and released,
        delay milliseconds apart. Letters, digits, punctuation, space,
        newline, tab, backspace and escape; anything else raises ValueError
        before a key is sent. Some apps lose keys that come faster than a
        person types (Notepad on Windows 11 does at 20 ms); delay=0
        sends as fast as the link takes them. Waits for room while a host
        is connected, and returns how many characters were queued: fewer
        than the text's length if the host leaves."""
    def mouse(self, x: int = 0, y: int = 0, *, buttons: int | None = None, wheel: int = 0) -> bool:
        """Move by x, y (right and down; any size, sent in steps of 127),
        turn the wheel (positive up), and set the buttons (bit 0 left, 1
        right, 2 middle), or keep them as they are with None. A click is
        `mouse(buttons=1)` then `mouse(buttons=0)`."""
    def gamepad(self, buttons: int = 0, *, x: int = 0, y: int = 0, z: int = 0, rz: int = 0,
                hat: int | None = None) -> bool:
        """Send the whole pad's state: buttons 1 to 16 as bits 0 to 15,
        the sticks from -127 to 127, and the hat from 0 (up) to 7
        clockwise, or None when centred."""
    def consumer(self, usage: int) -> bool:
        """Press and release a media key by its HID Consumer usage: 0xcd
        play/pause, 0xb5 next, 0xb6 previous, 0xe9 volume up, 0xea volume
        down, 0xe2 mute."""
    def changes(self) -> int:
        """What changed since the last call: `CONNECTION` and `HID` bits."""
    def stats(self) -> dict:
        """sent (reports), received, dropped (the stack refused one), and
        queued (bytes waiting)."""
    def deinit(self) -> None:
        """Drop the host and stop the device. The radio stays on."""
    def __enter__(self) -> HIDDevice: ...
    def __exit__(self, *args: object) -> None:
        """`deinit`."""


class HIDHost:
    """The board as the host of a Bluetooth keyboard, mouse or gamepad.

    Put the device in pairing mode, find it with `Bluetooth.inquiry`, and
    `connect` to its address; after that it reconnects by itself when it
    wakes. Read its input reports with `read`. One HID role runs at a time:
    deinit an `HIDDevice` first.

    ```python
    host = btclassic.HIDHost(bt)
    host.connect(address, boot=True)            # a keyboard, simply
    while True:
        report = host.read()
        if report:
            kind, *rest = host.decode(report) or ("other",)
            if kind == "keyboard":
                modifiers, keys = rest
                print("".join(host.char(k, modifiers) or "" for k in keys))
    ```

    Args:
        bt: the radio, `Bluetooth()`. It is turned on if it's off.
    """
    def __init__(self, bt: Bluetooth) -> None: ...
    connected: bool
    peer: bytes | None
    boot: bool
    """The device runs the boot protocol: its keyboard and mouse reports
    have the fixed boot layouts (ID 1 and 2), which `decode` reads."""
    descriptor: bytes | None
    """The device's HID report descriptor, once connected."""
    def connect(self, addr: bytes | None = None, *, boot: bool = False) -> None:
        """Call a device, or with no address the last one it was connected
        to, remembered across resets. boot asks for the boot protocol,
        which every keyboard and mouse has and needs no descriptor to read;
        gamepads have none. Connected, it switches protocol."""
    def disconnect(self) -> None: ...
    def read(self) -> bytes | None:
        """The next input report, oldest first, report ID first when the
        descriptor has IDs or the link runs the boot protocol; None with
        none waiting."""
    def send(self, report: bytes) -> None:
        """Send an output report, report ID first when the descriptor has
        IDs or the link runs the boot protocol: a keyboard's Caps Lock LED
        is `bytes((1, 2))`."""
    def decode(self, report: bytes) -> tuple | None:
        """Take a report apart, when its layout is known: in the boot
        protocol, or from a device whose descriptor is one of `HIDDevice`'s.
        None otherwise; read the descriptor and the bytes yourself.

        - ("keyboard", modifiers, (key, ...)): the keys held.
        - ("mouse", buttons, x, y, wheel)
        - ("consumer", usage): 0 when the key is released.
        - ("gamepad", buttons, hat, x, y, z, rz): hat None when centred.
        """
    @staticmethod
    def char(key: int, modifiers: int = 0) -> str | None:
        """What a key code types on a US layout, Shift in the modifiers
        honoured; None for keys that type nothing."""
    def changes(self) -> int:
        """What changed since the last call: `CONNECTION` and `HID` bits."""
    def stats(self) -> dict:
        """sent (output reports), received (input reports), dropped (the
        queue was full, or a send failed), and queued (bytes waiting)."""
    def deinit(self) -> None:
        """Drop the device and stop the host. The radio stays on."""
    def __enter__(self) -> HIDHost: ...
    def __exit__(self, *args: object) -> None:
        """`deinit`."""
