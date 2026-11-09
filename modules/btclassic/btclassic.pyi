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
`Serial.changes`, `HIDDevice.changes`, `HIDHost.changes`,
`PhoneBook.changes` and `AudioGateway.changes` bit: the link came up or went
down."""
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
other hands-free state changed. `AudioGateway.changes` bit: the headset
changed a gain, voice recognition or noise reduction."""
CALL: int
"""`HandsFree.changes` and `AudioGateway.changes` bit: a call started,
changed state or ended, or the caller is known."""
AUDIO: int
"""`HandsFree.changes` and `AudioGateway.changes` bit: the call's audio
came or went. `sample_rate` says its rate while it's up."""
PAIRED: int
"""`Bluetooth.changes` bit: a device paired, or a pairing was removed."""
FOUND: int
"""`Bluetooth.changes` bit: an inquiry found a device, or finished."""
PASSKEY: int
"""`Bluetooth.changes` bit: pairing has a passkey to show, type or check;
`Bluetooth.passkey` says which."""
IO_DISPLAY_ONLY: int
"""`config(io=...)`: the board can show a number but not answer yes or no."""
IO_DISPLAY_YESNO: int
"""`config(io=...)`: the board can show a number and answer yes or no."""
IO_KEYBOARD_ONLY: int
"""`config(io=...)`: the board can take a number typed, and show none."""
IO_NONE: int
"""`config(io=...)`: neither, so pairing is "just works". The default."""
PKT_DM1: int
"""`Bluetooth.tune` packet type: 1-slot, forward-error-corrected. Always
allowed, whether it is in the set or not."""
PKT_DH1: int
"""`Bluetooth.tune` packet type: 1-slot."""
PKT_DM3: int
"""`Bluetooth.tune` packet type: 3-slot, error-corrected."""
PKT_DH3: int
"""`Bluetooth.tune` packet type: 3-slot."""
PKT_DM5: int
"""`Bluetooth.tune` packet type: 5-slot, error-corrected."""
PKT_DH5: int
"""`Bluetooth.tune` packet type: 5-slot, the longest basic-rate packet."""
PKT_2DH1: int
"""`Bluetooth.tune` packet type: 1-slot at 2 Mbit/s."""
PKT_3DH1: int
"""`Bluetooth.tune` packet type: 1-slot at 3 Mbit/s."""
PKT_2DH3: int
"""`Bluetooth.tune` packet type: 3-slot at 2 Mbit/s."""
PKT_3DH3: int
"""`Bluetooth.tune` packet type: 3-slot at 3 Mbit/s."""
PKT_2DH5: int
"""`Bluetooth.tune` packet type: 5-slot at 2 Mbit/s."""
PKT_3DH5: int
"""`Bluetooth.tune` packet type: 5-slot at 3 Mbit/s."""
PKT_ALL: int
"""`Bluetooth.tune` packet types: every one, which is how a link starts."""
PASSKEY_INPUT: int
"""`Bluetooth.passkey` action: type the passkey the other device shows."""
PASSKEY_DISPLAY: int
"""`Bluetooth.passkey` action: show the passkey for the other side to type
or check. Nothing to answer."""
PASSKEY_COMPARE: int
"""`Bluetooth.passkey` action: show the number and say whether the other
device shows the same."""
SERIAL: int
"""`Serial.changes` bit: bytes arrived."""
DATA: int
"""`L2CAP.changes` bit: bytes arrived."""
REMOTE: int
"""`AudioSource.changes` bit: the receiver pressed a key; read it with
`AudioSource.key`."""
HID: int
"""`HIDDevice.changes` and `HIDHost.changes` bit: a report came, the host
set an output report (a keyboard's LEDs), the protocol changed, or there's
room to send again."""

PHONEBOOK: int
"""`PhoneBook.changes` bit: a request finished, whether or not it worked."""
REQUEST: int
"""`AudioGateway.changes` bit: the headset dialed a number (read `dialed`)
or sent a keypad digit (read `dtmf()`)."""
MESSAGES: int
"""`Messages.changes` bit: a request finished, whether or not it worked."""
NOTIFY: int
"""`Messages.changes` bit: the phone reported a change; read `event()`."""
LIBRARY: int
"""`Library.changes` bit: a request finished, whether or not it worked."""
TETHER: int
"""`Tether.changes` bit: the link to the phone came up, failed or went."""
PUSH: int
"""`ObjectPush.changes` bit: a send or pull finished, or an object came in
(read `received()`)."""

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
          keeps the board out of searches while that device is connected,
          and lists it again afterwards if this is True. Paired devices can
          still connect meanwhile, to Serial or any other service.
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
        - page_timeout (int): how long `connect` calls a device before it
          gives up, in milliseconds, 1 to 40959. Shorter finds out sooner
          that a device is away; 5120 by default on the ESP32, 15360 on the
          Pico W.
        - channel_map (bytes): the channels the board may hop on, 10 bytes
          with channel 0 (2402 MHz) in bit 0 of the first byte, through
          channel 78 (2480 MHz). At least 20 must be set; all 79 are by
          default. Clear the channels a Wi-Fi network sits on to keep out of
          its way.
        - min_key_size (int): the shortest encryption key, in bytes, 7 to
          16, the board lets a link use. 7 by default, the controller's own.
          Raise it to refuse a device that encrypts with a weak key. Waits
          for the controller to accept it, and raises OSError if it refuses.
          ESP32 only.
        - eir_uuids (bool): list the services running in the board's
          inquiry response, so a device searching sees what it offers before
          connecting. Off by default: the response has the name alone.
        - eir_tx_power (bool): add the transmit power, for a searching
          device to judge distance by. Off by default; ESP32 only.
        - eir_manufacturer (bytes or None): your own data in the inquiry
          response, a 2-byte company ID (little-endian) then up to 62 bytes.
        - eir_url (str or None): a URI in the inquiry response, up to 64
          bytes.
        - io (int): what the board has for pairing, one of `IO_NONE` (the
          default: "just works", nothing to show or type),
          `IO_DISPLAY_ONLY`, `IO_DISPLAY_YESNO` or `IO_KEYBOARD_ONLY`. With
          anything but `IO_NONE`, pairing asks for a passkey or a number
          check through `passkey`, and a `Serial` link asks the other side
          for one too. Set it before starting the profiles.

        Raises:
            ValueError: an unknown setting, or a value it doesn't take.
        """
    def paired(self) -> list[bytes]:
        """The addresses of the devices the board is paired with. Pairings
        are kept in flash and survive a reset."""
    def unpair(self, addr: bytes | None = None) -> None:
        """Forget a paired device, or every one with no argument. The
        device has to pair again before it can connect. An address the
        board isn't paired with is left alone, without an error."""
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
    def eir(self, addr: bytes) -> bytes | None:
        """The extended inquiry response a device sent when the last
        `inquiry` found it, as it came: length, type and data fields, as in
        the Bluetooth Core Specification. None when it sent none. ESP32
        only.

        Raises:
            ValueError: the last inquiry didn't find that device.
        """
    def rssi(self, addr: bytes) -> int:
        """How far a connected device's signal is from the range the
        controller wants, in dB: 0 inside it, negative weaker, positive
        stronger. Any profile's link counts. Waits up to a second for the
        answer.

        Raises:
            OSError: no link to that device is up.
        """
    def tune(self, addr: bytes, *, poll_ms: int | None = None, packet_types: int | None = None) -> dict[str, int]:
        """Set how one connected device's link is run, and say what the
        controller reports back for each setting given. A link starts with
        every packet type allowed and a poll interval the two devices agree.
        Waits up to two seconds for each answer.

        Args:
            addr: the device's address.
            poll_ms: the longest the master may go without sending to the
                slave, 4 to 2560 ms. A shorter one keeps a keyboard or a
                game pad responsive and costs air time; a longer one saves
                power. Either end of the link can ask.
            packet_types: the packet types the link may use, `PKT_` bits
                added together: `PKT_DH1 | PKT_DH3` keeps to 1- and 3-slot
                packets. Short, error-corrected ones (`PKT_DM1`, `PKT_DM3`)
                ride out interference; the long 3 Mbit/s ones carry the most.
                `PKT_DM1` is allowed whether it is listed or not.

        Returns:
            A dict with `"poll_ms"` and `"packet_types"` for the settings
            given: the values the controller reports in force.

        Raises:
            OSError: no link to that device is up, the controller refused,
                or this firmware doesn't have the command.
            ValueError: a value out of range.
    def sniff(self, addr: bytes, enable: bool = True, *, min_ms: int = 250, max_ms: int = 500,
        attempt: int = 4, timeout: int = 1) -> int:
        """Put a connected device's idle link into sniff mode, where it wakes
        only every `min_ms` to `max_ms`, or with `enable` False wake it
        again. `attempt` and `timeout` are baseband slots (0.625 ms) spent
        listening at each wake-up. Returns the interval the link now wakes
        at, in ms, or 0 when it's active. Waits up to 3 s, and up to 8 s to
        wake a link that is subrating. Pico W only.

        Raises:
            OSError: no link to that device is up, the controller or the
                other side refused, or the board can't ask (ESP32).
        """
    def subrate(self, addr: bytes, *, max_latency_ms: int = 2000, remote_timeout_ms: int = 0,
        local_timeout_ms: int = 0) -> int:
        """Let a sniffing link skip wake-ups while it's quiet, up to
        `max_latency_ms`. Returns the latency the two sides agreed on, in
        ms. Pico W only.

        Raises:
            OSError: no link, or the other side doesn't do subrating.
        """
    def link_mode(self, addr: bytes) -> tuple[str, int]:
        """A connected device's link mode, "active", "hold" or "sniff", and
        the sniff interval in ms (0 when it isn't sniffing). Either side
        can start sniff, so this tells you where a link stands.

        Raises:
            OSError: no link to that device is up (Pico W).
        """
    def services(self, addr: bytes, *, timeout_ms: int = 20000) -> list[int | bytes]:
        """The services a device offers, read from its SDP server: each
        one's service class UUID, an int for a 16- or 32-bit one (0x110B is
        an audio sink, 0x1101 a serial port) and 16 bytes, most significant
        first, for a 128-bit one. The board calls the device if no link is
        up, which can take the page timeout. Waits up to `timeout_ms`.
        On the ESP32 the 16-bit services listed are the profiles Bluedroid
        knows, which take in audio, calls, serial, HID, PAN, OBEX, the
        phone book and messages; Pico W lists every one.

        Raises:
            OSError: the radio is off, another search is running, the device
                didn't answer, or ETIMEDOUT.
        """
    def passkey(self) -> tuple[bytes, int, int | None] | None:
        """The pairing step waiting, after `changes` has had `PASSKEY`:
        (address, action, passkey), or None. The action is
        `PASSKEY_COMPARE`, `PASSKEY_DISPLAY` or `PASSKEY_INPUT`; the passkey
        is the six-digit number to show, and None for `PASSKEY_INPUT`."""
    def passkey_reply(self, accept: bool, passkey: int | None = None) -> None:
        """Answer the step waiting: for `PASSKEY_COMPARE` whether the
        numbers match, for `PASSKEY_INPUT` the passkey typed (0 to 999999).
        `accept` False refuses the pairing.

        Raises:
            OSError: no step waits for an answer.
            ValueError: the passkey is out of range.
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

    In a firmware with audiodsp, the sink is also an audiosample: hand it to
    the audio pump, alone or into a `Mixer` beside a synth, and the pump
    pulls the music in C on its own task. Nothing your program does then
    holds the audio up: not a garbage collection, not a cover decoding, not
    a redraw. Read it either way, not both: one reader per stream.

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
            that redraws a screen between reads wants 1000. Played by the
            audio pump, nothing reads late, and 500 is plenty. The default
            depends on the board: 500 where there is PSRAM, 250 otherwise.
        reconnect: call the last sender back, if it is still paired, as a
            speaker does: when the sink starts, and whenever the link drops
            (the sender went out of range, or its Bluetooth went off),
            every few seconds for about a minute. It stops early when the
            sender has forgotten the board, so the sender can pair again.
            `disconnect` doesn't call back.
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
    44100 almost always; A2DP allows 16000, 32000 and 48000 too. The pump
    pulls at the rate this last read, so play the sink after the `FORMAT`
    bit, and again when it comes back with a new rate."""
    channel_count: int
    """2: the sink's frames are stereo. An audiosample's field."""
    bits_per_sample: int
    """16. An audiosample's field."""
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
        """Drop the sender, without calling it back. The board is
        discoverable again afterwards."""
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
    `write` takes your microphone. In a firmware with audiodsp, the
    hands-free unit is also an audiosample the audio pump pulls in C, as
    `AudioSink` is, and `uplink` sends the microphone from that same pull, so
    a call needs no Python in its path either way. The phone decides when the audio comes
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
    up. The pump pulls at the rate this last read: play the hands-free unit
    after the `AUDIO` bit says the call's audio is up."""
    channel_count: int
    """`channels`, as given. An audiosample's field."""
    bits_per_sample: int
    """16. An audiosample's field."""

    def __init__(self, bt: Bluetooth, *, buffer_ms: int = 400, channels: int = 1) -> None: ...
    def readinto(self, buf: bytearray | memoryview) -> int:
        """Fill `buf` with the phone's voice: 16-bit frames of `channels`
        at `sample_rate`, scaled by `speaker_gain`. Never waits: silence
        fills whatever the call can't, and all of it while `audio` is False.

        Read a block when your output has room for one, or by the clock at
        `sample_rate`, never just because the call has data: a reader that
        runs ahead empties the buffer, which then refills to half before
        playing on and repeats frames while it is low.

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
    def uplink(self, mic: object) -> None:
        """Send the board's microphone to the phone from the audio pump's
        pull of this hands-free unit, with no Python in the path; None
        stops it, and the phone hears silence. While it runs, nothing else
        may read the microphone, and `write` would interleave with it.

        The microphone and the speaker share the output's clock, so the
        call keeps one pace both ways.

        Args:
            mic: the open `pcm_in` the microphone records with, at the
                call's `sample_rate`, one 16-bit channel.

        Raises:
            ValueError: `mic` records another shape, or at another rate.
            OSError: this firmware has no audio pump or no capture.
        """
    def changes(self) -> int:
        """The bits (`CONNECTION`, `PHONE`, `CALL`, `AUDIO`) for what changed
        since the last call, which clears them."""
    def stats(self) -> dict[str, int]:
        """Counters for the call's buffer, as `AudioSink.stats` has them.
        Also `sent`, the microphone frames `uplink` or `write` queued for
        the phone during a call, and `unsent`, those the queue had no room
        for."""
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
    link and all; its `name` and `rxbuf` then change nothing. It runs
    beside an `L2CAP` channel; on a classic ESP32, start it first (see
    `L2CAP`).

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

class L2CAP:
    """A raw L2CAP channel, as a stream: `read`, `readinto`, `readline` and
    `write` work as on `machine.UART`, and `select.poll` takes it.

    For a protocol of your own between two devices, with no serial port
    framing in the way. Making it with a `psm` serves that PSM for other
    devices to connect to; `connect` calls another device's. One link at a
    time. Your own protocols take odd PSMs from 0x1001 up, such as 0x1001,
    0x1003 or 0x2001.

    There is one channel. A second `L2CAP` with the same `psm` takes over
    the running one, link and all; one with another `psm` closes it first.
    It runs beside a `Serial` port. On a classic ESP32, starting the
    `Serial` port drops an open channel, which is then served again, so
    start the `Serial` port first. ESP32 and Pico W.

    Args:
        bt: the radio, `Bluetooth()`. It is turned on if it's off.
        psm: the PSM to serve, or None to only call out.
        rxbuf: bytes held for `read`, 256 to 65536. Bytes that arrive with
            it full are dropped and counted in `stats`.
        timeout: how long `read` waits for its first byte, in ms.
    """

    connected: bool
    """A device is connected."""
    listening: bool
    """The PSM is served, waiting for a device to connect."""
    peer: bytes | None
    """The connected device's address, or None."""
    psm: int | None
    """The PSM served, or None."""
    mtu: int | None
    """The most bytes one packet to the peer carries, or None with no link.
    `write` splits what you give it into packets of this size."""
    timeout: int
    """How long `read` waits for its first byte, in ms. Settable."""

    def __init__(self, bt: Bluetooth, psm: int | None = None, *, rxbuf: int = 4096, timeout: int = 0) -> None: ...
    def read(self, nbytes: int = -1) -> bytes | None:
        """What has arrived, up to `nbytes`, or None when nothing came
        within `timeout`."""
    def readinto(self, buf: bytearray | memoryview, nbytes: int = -1) -> int | None: ...
    def readline(self) -> bytes | None: ...
    def write(self, buf: bytes | bytearray | memoryview) -> int:
        """Send `buf`. Waits while the link is up until all of it is
        queued, so the link's pace sets yours.

        Raises:
            OSError: no device is connected.
        """
    def any(self) -> int:
        """How many bytes are waiting to be read."""
    def changes(self) -> int:
        """The bits (`CONNECTION`, `DATA`) for what changed since the last
        call, which clears them."""
    def connect(self, addr: bytes, psm: int, timeout_ms: int = 10000) -> None:
        """Call another device's `psm` and wait until the channel is open.

        Raises:
            OSError: "no answer from the device" when it can't be reached
                or turns the channel down, ETIMEDOUT after `timeout_ms`.
        """
    def disconnect(self) -> None: ...
    def stats(self) -> dict[str, int]:
        """Counters: rx_bytes, tx_bytes, and rx_dropped (bytes that found
        the receive buffer full)."""
    def deinit(self) -> None:
        """Drop the link and stop serving the PSM. The radio stays on."""
    def __enter__(self) -> L2CAP: ...
    def __exit__(self, *args: object) -> None:
        """`deinit`."""

class Service:
    """An SDP record of your own, so a `Serial` port or an `L2CAP` channel
    shows up under a UUID and a name you choose.

    A device that searches a board's services, as `Bluetooth.services` does,
    lists what the board's profiles offer. A `Serial` port is listed as
    the standard serial port, 0x1101, under the name given to `Serial`.
    A `Service` adds a record beside that one, for a program that wants to be
    found by a UUID of its own: a phone app or a computer asks for it, then
    connects to the channel the record names. The record is dropped by
    `deinit`, and when the radio turns off. Up to four at once.

    A record with a `channel` lists RFCOMM as its protocol, which is what
    computers and phones look for. One with only a `psm` carries the PSM in
    the record's "GOEP L2CAP PSM" attribute, 0x0200, and lists no RFCOMM
    channel, so a computer's serial port lookup skips it.

    Args:
        bt: the radio, `Bluetooth()`. It is turned on if it's off.
        uuid: what the service is: an int for a 16- or 32-bit UUID, or 16
            bytes, most significant first, for a 128-bit one. Make your own
            128-bit UUID rather than borrowing an assigned one.
        name: the service name devices list, 1 to 31 bytes.
        channel: the RFCOMM channel to point at, 1 to 30: a `Serial`
            port's `channel`.
        psm: the L2CAP PSM to point at: an `L2CAP` channel's `psm`.

    Raises:
        OSError: the stack couldn't make the record, there are four already,
            or this firmware doesn't have it.
        ValueError: neither `channel` nor `psm`, or a value out of range.
    """

    active: bool
    """The record is being served."""

    def __init__(self, bt: Bluetooth, uuid: int | bytes, name: str, *, channel: int | None = None, psm: int | None = None) -> None: ...
    def deinit(self) -> None:
        """Remove the record. The channel it pointed at stays up."""
    def __enter__(self) -> Service: ...
    def __exit__(self, *args: object) -> None:
        """`deinit`."""

class Library:
    """The media library of the phone `AudioSink` plays from (AVRCP
    browsing): its players, a player's folders and tracks, its queue and a
    search, and a track to play.

    Items are dicts. A folder has "uid", "name", "folder" (True) and
    "playable"; a track has "uid", "name", "folder" (False), "video", and
    "artist" and "album" when the phone sends them. A player has "id",
    "name", "playing" and "browsable". Pass a uid to `enter` or `play`.

    Only the Pico W has it. On the ESP32 the class is there, and making one
    raises OSError: its Bluetooth stack can't browse.

    Args:
        bt: the radio, `Bluetooth()`. It is turned on if it's off.
        max_bytes: the most one reply may take; a reply past it is cut, and
            `truncated` says so.
    """

    connected: bool
    """The browsing link to the phone is up."""
    peer: bytes | None
    """The phone's address, or None."""
    depth: int | None
    """Folders below the browsed player's root, or None before `browse`."""
    truncated: bool
    """The last reply outgrew `max_bytes`; ask for fewer items at a time."""

    def __init__(self, bt: Bluetooth, *, max_bytes: int = 16384) -> None: ...
    def connect(self, *, timeout: int = 10000) -> None:
        """Open browsing to the phone `AudioSink` is connected to.

        Raises:
            OSError: no `AudioSink` link, or the phone refused.
        """
    def disconnect(self) -> None: ...
    def players(self, *, timeout: int = 5000) -> list[dict]:
        """The phone's media players: the apps it can play from."""
    def browse(self, player: int, *, timeout: int = 5000) -> list[str]:
        """Browse a player by its "id": returns the names of the folders
        down to where it is. Some apps can be browsed only while they're
        open on the phone."""
    def items(self, offset: int = 0, count: int = 20, *, scope: str = "folder", timeout: int = 10000) -> list[dict]:
        """A page of items. `scope` is "folder" (the current folder),
        "search" (the last search's results) or "queue" (what's lined up
        to play). Past the end, the list is empty."""
    def enter(self, uid: bytes) -> None:
        """Go into a folder."""
    def up(self) -> None:
        """Go up one folder."""
    def search(self, text: str) -> None:
        """Search the browsed player; read the results with
        `items(scope="search")`."""
    def play(self, uid: bytes, *, scope: str = "folder") -> None:
        """Play an item, from the scope it was listed in."""
    def changes(self) -> int:
        """The bits (`CONNECTION`, `LIBRARY`) for what changed since the
        last call, which clears them."""
    def deinit(self) -> None:
        """Drop the browsing link. The radio and the sink stay on."""
    def __enter__(self) -> Library: ...
    def __exit__(self, *args: object) -> None:
        """`deinit`."""


class Tether:
    """The internet through a phone's Bluetooth tethering (PAN), as a
    network interface beside the Wi-Fi's: once it's connected, `socket`,
    `requests` and `ntptime` use it as they would `network.WLAN`.

    The phone hands out the address by DHCP, and the tether becomes the
    default route while it's up; the route goes back to the Wi-Fi when it
    drops. Turn on Bluetooth tethering in the phone's hotspot settings first.

    Only the Pico W has it. On the ESP32 the class is there, and making one
    raises OSError: its Bluetooth stack has no PAN.

    Args:
        bt: the radio, `Bluetooth()`. It is turned on if it's off.
    """

    connected: bool
    """The Bluetooth link to the phone is up (it may not have an address
    yet; `isconnected()` says it has)."""
    peer: bytes | None
    """The phone's address, or None."""

    def __init__(self, bt: Bluetooth) -> None: ...
    def connect(self, addr: bytes | None = None, *, timeout: int = 30000) -> None:
        """Join the phone's tethering, and wait until the phone has given the
        board an address. With no address, the last device connected, or else
        the first paired one. `timeout=0` returns at once; watch for `TETHER`
        and `isconnected()`.

        Raises:
            OSError: the phone refused (its Bluetooth tethering is off), or
                ETIMEDOUT.
        """
    def disconnect(self) -> None: ...
    def isconnected(self) -> bool:
        """Up, with an address from the phone."""
    def ifconfig(self, config: tuple[str, str, str, str] | None = None) -> tuple[str, str, str, str]:
        """As `network.WLAN.ifconfig`: address, netmask, gateway, DNS."""
    def ipconfig(self, *args: str, **kwargs: object) -> object:
        """As `network.WLAN.ipconfig`: `ipconfig("addr4")`, `ipconfig("gw4")`."""
    def stats(self) -> dict[str, int]:
        """Counters: rx_frames, tx_frames, and tx_dropped (frames the link
        had no room for, which TCP sends again)."""
    def changes(self) -> int:
        """The bits (`TETHER`) for what changed since the last call, which
        clears them."""
    def deinit(self) -> None:
        """Drop the link and take the interface down. The radio stays on."""
    def __enter__(self) -> Tether: ...
    def __exit__(self, *args: object) -> None:
        """`deinit`."""


class ObjectPush:
    """Files and contact cards over Bluetooth (OPP): send one to a phone or
    computer, pull its owner's card, or take what it sends.

    A send or pull opens a session to the device, moves the one object and
    closes it. While `listen()` is on, the board offers Object Push to paired
    devices and keeps one received object at a time: others are turned away
    until `received()` takes it.

    The ESP32 and the Pico W have it. On a firmware built without it the
    class is there, and making one raises OSError.

    Args:
        bt: the radio, `Bluetooth()`. It is turned on if it's off.
        max_bytes: the most a pulled or received object may take; past it the
            object is cut, and `truncated` says so.
    """

    busy: bool
    """A send or pull is running."""
    listening: bool
    """`listen()` is on."""
    receiving: bool
    """A device is sending an object now."""
    peer: bytes
    """The address of the last device sent to, pulled from or received from."""
    sent: int
    """Bytes of the last send that went."""
    truncated: bool
    """The last pulled or received object outgrew `max_bytes`."""
    refused: int
    """Objects turned away while one waited to be taken."""

    def __init__(self, bt: Bluetooth, *, max_bytes: int = 65536) -> None: ...
    def send(self, name: str, data: bytes, *, addr: bytes | None = None, type: str | None = None,
             timeout: int = 60000) -> None:
        """Send `data` as a file called `name`, with a MIME `type` such as
        "text/x-vcard" for a contact card. Without `addr`, it goes to the
        last device the board talked to, or the first paired one. A phone
        usually asks its user to accept it.

        Raises:
            OSError: the device refused it, or couldn't be reached; or
                ETIMEDOUT.
        """
    def pull(self, *, addr: bytes | None = None, timeout: int = 30000) -> bytes:
        """The device owner's contact card. Many phones don't give one.

        Raises:
            OSError: the device has nothing to give, or refused.
        """
    def listen(self, on: bool = True) -> None:
        """Offer Object Push to paired devices, or stop."""
    def received(self) -> tuple[str, str | None, bytes] | None:
        """The name, MIME type and contents of the object a device sent, or
        None. Taking it lets the next one in."""
    def changes(self) -> int:
        """The bits (`CONNECTION`, `PUSH`) for what changed since the last
        call, which clears them."""
    def deinit(self) -> None:
        """Stop listening and drop what's held. The radio stays on."""
    def __enter__(self) -> ObjectPush: ...
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
        ids: (vendor, product) or (vendor, product, version), published
            in a Device ID record so a host can tell which product this
            is, as it does a USB device by its IDs. The vendor is a USB
            vendor ID. None publishes no record. A host that has already
            paired with the board may keep the IDs it learned then.
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
                 name: str | None = None, boot: bool | None = None,
                 ids: tuple[int, ...] | None = None) -> None: ...
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
    def set_feature(self, report: bytes) -> None:
        """Give a feature report its value, report ID first when the
        descriptor has IDs: a setting the host reads when it asks, such as
        a firmware version or a sensitivity. Up to four IDs. The host can
        change one you have set; that raises `HID`, and `feature` reads it.
        The host can't add an ID you haven't set."""
    def feature(self, report_id: int = 0) -> bytes | None:
        """A feature report as it stands, ID first when it has one, or None
        if neither you nor the host has set it."""
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
    ids: tuple[int, int, int] | None
    """The device's (vendor, product, version) from its service record,
    once connected: what tells one gamepad from another, to map its
    buttons. None before, or when the device publishes no Device ID
    record. An ESP32 keeps what a device published when they first
    paired, even through `unpair`: a device that changes or drops its
    record later still shows the IDs it had then. A Pico W board reads
    the record again on each connection."""
    INPUT: int
    OUTPUT: int
    FEATURE: int
    """Report types, for `get_report` and `set_report`."""
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
    def get_report(self, report_id: int = 0, type: int = 3, *, timeout: int = 1000) -> bytes:
        """Ask the device for one report and wait for it, report ID first
        when it has one. Most often a `FEATURE` report, which some gamepads
        need read before they send their full input reports.

        Raises:
            OSError: ETIMEDOUT after timeout ms, or the device refused.
        """
    def set_report(self, report: bytes, type: int = 3, *, timeout: int = 1000) -> None:
        """Set one report on the device, report ID first when the
        descriptor has IDs: a `FEATURE` report by default, or an `OUTPUT`
        report sent the slower, acknowledged way. Waits up to timeout
        milliseconds for the device to accept it: OSError if it refuses,
        ETIMEDOUT if it doesn't answer."""
    def decode(self, report: bytes) -> tuple | None:
        """Take an input report apart by what its descriptor says each
        field means, so any keyboard, mouse, gamepad or media remote reads
        the same. None for a report it can't place (a vendor's own); read
        the descriptor and the bytes yourself then.

        - ("keyboard", modifiers, (key, ...)): the keys held.
        - ("mouse", buttons, x, y, wheel)
        - ("consumer", usage): 0 when the key is released.
        - ("gamepad", buttons, hat, x, y, z, rz): buttons 1 to 32 as bits
          0 to 31, hat 0 to 7 clockwise from up and None when centred, the
          axes on -127..127 whatever range the device sends.
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


class PhoneBook:
    """A phone's contacts and call history (PBAP client).

    `connect` opens the phone's phone book; the phone asks its owner the
    first time, so allow access to contacts on the phone. Each request
    then waits for the whole reply.

    A book is named as the phone names it: "pb" for contacts, "ich"
    received calls, "och" dialed calls, "mch" missed calls, "cch" all
    three, "spd" speed dial and "fav" favourites; `sim=True` reads the
    SIM's copy. Phones keep the first two or five, as `repositories` says.

    A record is a dict: "name", "numbers" as a list of (number, kind)
    pairs where kind is "cell", "home", "work", "main", "fax", "pager",
    "other" or "", and "emails". "org" is there when the card has one.
    Call history adds "call" ("missed", "received" or "dialed") and
    "time" ("20261108T091500", the phone's local time).

    One phone book at a time; a second `PhoneBook` takes over the running
    one.

    Args:
        bt: the radio, `Bluetooth()`. It is turned on if it's off.
        max_bytes: the largest reply kept, 1024 or more. A longer reply
            stops there and sets `truncated`; page with `offset` and
            `count` instead. Kept in PSRAM when the board has it.
    """

    connected: bool
    """The phone book is open."""
    peer: bytes | None
    """The phone's address, or None."""
    repositories: list[str]
    """Where the phone keeps books: "phone", "sim", "speed_dial",
    "favorites"."""
    features: int
    """The phone's PBAP feature bits."""
    new_missed: int | None
    """Missed calls the phone hasn't shown yet, from the last pull of a
    call history, or None. Always None on the Pico W, whose Bluetooth
    stack doesn't pass the count on."""
    truncated: bool
    """The last reply was longer than `max_bytes` and was cut."""

    def __init__(self, bt: Bluetooth, *, max_bytes: int = 262144) -> None: ...
    def connect(self, addr: bytes | None = None, *, timeout: int = 30000) -> None:
        """Open the phone book of the phone at `addr`; None picks the phone
        `HandsFree` or `AudioSink` is connected to, then the first paired
        device. Waits up to `timeout` ms for the link; 0 returns at once,
        and `changes` reports `CONNECTION`.

        Raises:
            OSError: the phone refused, or the wait timed out.
        """
    def disconnect(self) -> None: ...
    def pull(self, book: str = "pb", *, offset: int = 0, count: int | None = None, raw: bool = False,
             photos: bool = False, sim: bool = False, version: int = 21,
             timeout: int = 30000) -> list[dict] | bytes:
        """The book's records from `offset`, `count` of them or all. With
        `raw` the vCards as the phone sent them. `version` asks for vCard
        2.1 (21) or 3.0 (30); `photos` adds each contact's picture to the
        raw vCards."""
    def size(self, book: str = "pb", *, sim: bool = False, timeout: int = 30000) -> int | None:
        """How many entries the book holds, without pulling them."""
    def list(self, book: str = "pb", *, offset: int = 0, count: int | None = None, search: str | None = None,
             by: str = "name", order: str = "indexed", sim: bool = False,
             timeout: int = 30000) -> list[tuple[str, str]]:
        """The book's listing: (handle, name) pairs, where name is the
        phone's "Family;Given" form. `search` keeps the entries whose
        `by` ("name" or "number") matches; `order` is "indexed" or
        "name". Pass a handle to `entry`."""
    def entry(self, book: str, handle: str, *, raw: bool = False, photos: bool = False,
              sim: bool = False, version: int = 21, timeout: int = 30000) -> dict | bytes | None:
        """One record by its handle from `list`, such as "5.vcf"."""
    def lookup(self, number: str, *, timeout: int = 10000) -> str | None:
        """The contact name for a phone number, or None: what a call screen
        shows for `HandsFree.caller`. The phone does the matching."""
    @staticmethod
    def parse(data: bytes) -> list[dict]:
        """Records from vCard text, a .vcf file's for one."""
    def changes(self) -> int:
        """The bits (`CONNECTION`, `PHONEBOOK`) for what changed since the
        last call, which clears them."""
    def deinit(self) -> None:
        """Close the phone book. The radio stays on."""
    def __enter__(self) -> PhoneBook: ...
    def __exit__(self, *args: object) -> None:
        """`deinit`."""


class AudioGateway:
    """The board as the phone, for a headset or car kit (HFP audio
    gateway).

    A headset pairs with the board, or `connect` calls one. The board
    reports a network, signal and battery, rings with a caller's number,
    and takes answer and hang-up from the headset's button. While `audio`
    is True, `write` sends 16-bit mono to the headset's speaker and
    `readinto` reads its microphone, at `sample_rate` (8 or 16 kHz).

    The board runs one hands-free role at a time: `deinit` a `HandsFree`
    before making an `AudioGateway`, and the other way round.

    On the Pico W it is control only: the CYW43439 never completes a call
    audio link, so `audio` stays False, `connect_audio` raises OSError,
    `in_band_ring` can't be turned on, and `readinto` and `write` move
    nothing. `ring`, `dial`, `answer`, `hangup`, the indicators and the
    volumes all work.

    Args:
        bt: the radio, `Bluetooth()`. It is turned on if it's off.
        buffer_ms: audio held each way, 50 to 1000 ms.
        channels: what `readinto` fills, 1 or 2 (the same sound in both).
    """

    connected: bool
    """A headset is connected."""
    peer: bytes | None
    """The headset's address, or None."""
    features: int
    """The headset's HFP feature bits."""
    service: bool
    """The network is up. Settable."""
    signal: int | None
    """Signal strength 0 to 5. Settable."""
    roaming: bool
    """Settable."""
    battery: int | None
    """Battery 0 to 5. Settable."""
    operator: str | None
    """The network's name the headset shows. Settable."""
    number: str | None
    """The board's own number, which a headset may ask for. Settable."""
    call: str
    """"idle", "incoming", "dialing", "alerting", "active" or "held"."""
    caller: str | None
    """The number of the incoming call."""
    dialed: str | None
    """The number the headset last dialed."""
    in_band_ring: bool
    """The ring is sent as audio instead of the headset's own tone.
    Settable."""
    voice_recognition: bool
    """Voice recognition is on, from the headset's button or set here.
    Settable."""
    nrec: bool
    """The headset asked for noise reduction and echo cancelling."""
    speaker_gain: int | None
    """The headset's speaker volume, 0 to 15. Settable."""
    mic_gain: int | None
    """The headset's microphone gain, 0 to 15. Settable."""
    audio: bool
    """The call's audio link is up."""
    sample_rate: int
    """8000 or 16000 while `audio`, 0 otherwise."""
    wideband: bool
    """The audio is wideband (mSBC, 16 kHz)."""

    def __init__(self, bt: Bluetooth, *, buffer_ms: int = 400, channels: int = 1) -> None: ...
    def connect(self, addr: bytes | None = None) -> None:
        """Call the headset at `addr`, or the last one. The link comes up
        later; watch for `CONNECTION`."""
    def disconnect(self) -> None: ...
    def connect_audio(self) -> None:
        """Open the audio link outside a call, for voice recognition or a
        test. A call opens it on its own."""
    def disconnect_audio(self) -> None: ...
    def ring(self, number: str | None = None) -> None:
        """An incoming call from `number`. The headset rings until it
        answers (`answer` here works too) or `hangup`."""
    def dial(self, number: str) -> None:
        """An outgoing call: `call` goes to "dialing" and the audio opens.
        `alerting` then `answer` play the far end picking up."""
    def alerting(self) -> None:
        """The outgoing call is ringing at the far end."""
    def answer(self) -> None:
        """The call is up: answer an incoming one, or the far end answered
        an outgoing one."""
    def hangup(self) -> None:
        """End the call, or reject it while it rings."""
    def dtmf(self) -> str:
        """Keypad digits the headset sent since the last call."""
    def readinto(self, buf: bytearray | memoryview) -> int:
        """Fill `buf` with the headset's microphone, 16-bit, `channels` per
        frame. Returns frames; silence fills what hasn't arrived. Read at
        your output's pace, as with `HandsFree.readinto`."""
    def write(self, buf: bytes | bytearray | memoryview) -> int:
        """Queue 16-bit mono for the headset's speaker. Returns the bytes
        taken, fewer when the buffer is full."""
    def stats(self) -> dict[str, int]:
        """The microphone ring's counters (buffered, capacity, received,
        underruns, overruns, dropped, repeated, skipped), and sent and
        silent: frames sent to the headset, and those sent as silence
        because `write` fell behind."""
    def changes(self) -> int:
        """The bits (`CONNECTION`, `PHONE`, `CALL`, `AUDIO`, `REQUEST`) for
        what changed since the last call, which clears them."""
    def deinit(self) -> None:
        """Drop the headset and stop the gateway. The radio stays on."""
    def __enter__(self) -> AudioGateway: ...
    def __exit__(self, *args: object) -> None:
        """`deinit`."""


class Messages:
    """A phone's text messages (MAP client): read, mark, delete, send, and
    hear about new ones.

    `connect` finds the phone's message service and opens it; the phone
    asks its owner the first time, so allow access to messages on the
    phone. Each request then waits for the whole reply.

    Folders are named under the phone's message root: "inbox", "outbox",
    "sent", "deleted" and "draft". A name with a "/" in it is a full
    path, "telecom/msg/inbox".

    A listing is a list of dicts, one per message, with the phone's own
    fields as strings: "handle" (what `get`, `mark_read` and `delete`
    take), "subject", "datetime" ("20261108T091500"), "sender_name",
    "sender_addressing", "recipient_addressing", "type" ("SMS_GSM",
    "MMS", "EMAIL"), "size", "read" ("yes" or "no") and more.

    One message service at a time; a second `Messages` takes over the
    running one.

    Args:
        bt: the radio, `Bluetooth()`. It is turned on if it's off.
        max_bytes: the largest reply kept, 1024 or more. A longer reply
            stops there and sets `truncated`. Kept in PSRAM when the
            board has it.
    """

    connected: bool
    """The message service is open."""
    peer: bytes | None
    """The phone's address, or None."""
    instance: int | None
    """The phone's ID for the open message service, or None."""
    features: int
    """The phone's MAP feature bits."""
    types: list[str]
    """What the open service carries: "email", "sms_gsm", "sms_cdma",
    "mms", "im"."""
    new_message: bool | None
    """The phone's new-message flag from the last listing, or None."""
    time: str | None
    """The phone's clock at the last listing, "20261108T101500+0000", or
    None."""
    truncated: bool
    """The last reply was longer than `max_bytes` and was cut."""
    notifying: bool
    """The phone agreed to report changes."""
    notify_connected: bool
    """The phone's link for reports is open. A phone that agrees to
    report changes opens it a moment later."""
    events: int
    """Reports waiting for `event()`."""

    def __init__(self, bt: Bluetooth, *, max_bytes: int = 262144) -> None: ...
    def connect(self, addr: bytes | None = None, *, instance: int | None = None,
                timeout: int = 30000) -> None:
        """Open the message service of the phone at `addr`; None picks the
        phone `HandsFree` or `AudioSink` is connected to, then the first
        paired device. `instance` picks one of several services by its ID;
        None takes the first with texts. Waits up to `timeout` ms for the
        link; 0 returns at once, and `changes` reports `CONNECTION`.

        Raises:
            OSError: the phone has no message service, refused, or the
                wait timed out.
        """
    def disconnect(self) -> None: ...
    def folders(self, folder: str | None = None, *, timeout: int = 30000) -> list[str]:
        """The names of the folders in `folder`, the message root when
        None."""
    def list(self, folder: str = "inbox", *, offset: int = 0, count: int | None = None,
             unread: bool | None = None, sms_only: bool = False, subject_length: int = 0,
             timeout: int = 30000) -> list[dict]:
        """The folder's messages from `offset`, `count` of them or all,
        newest first. `unread` True keeps only unread ones, False only read
        ones. `subject_length` asks for up to that many characters of each
        message's start as "subject" (1 to 255; 0 lets the phone choose)."""
    def size(self, folder: str = "inbox", *, unread: bool | None = None, sms_only: bool = False,
             timeout: int = 30000) -> int | None:
        """How many messages the folder holds, without listing them."""
    def get(self, handle: str, *, raw: bool = False, attachments: bool = False,
            timeout: int = 30000) -> dict | bytes:
        """One message as a dict: "status" ("READ" or "UNREAD"), "type",
        "folder", "sender" (a record as `PhoneBook` gives, or None),
        "recipients" (records) and "text". With `raw`, the bMessage as the
        phone sent it. Reading a message may mark it read on the phone."""
    def mark_read(self, handle: str, read: bool = True) -> None:
        """Mark a received message read or unread. Android refuses this
        for messages in "draft", "sent" and "outbox"."""
    def delete(self, handle: str, deleted: bool = True) -> None:
        """Move the message to the phone's deleted folder, or back out of
        it with `deleted=False`."""
    def send(self, number: str, text: str, *, folder: str = "outbox",
             timeout: int = 30000) -> str | None:
        """Send a text message to `number` and return the phone's handle
        for it. In "outbox" the phone sends it at once; in "draft" it keeps
        it there unsent."""
    def push(self, bmessage: bytes, *, folder: str = "outbox", retry: bool = True,
             timeout: int = 30000) -> str | None:
        """Hand the phone a whole bMessage, for what `send` can't say.
        Returns the phone's handle for it."""
    def update_inbox(self) -> None:
        """Ask the phone to fetch new mail now (email services only)."""
    def notify(self, on: bool = True) -> None:
        """Ask the phone to report changes as they happen. `changes`
        reports `NOTIFY` for each, and `event()` reads them.
        `notify_connected` says whether the phone has opened its link for
        reports, a moment after it agrees.

        Raises:
            OSError: the board couldn't advertise where the phone should
                send its reports."""
    def event(self) -> dict | None:
        """The oldest waiting report from the phone, or None. A dict of its
        fields as strings: "type" ("NewMessage", "MessageDeleted",
        "MessageShift", "ReadStatusChanged", "SendingSuccess",
        "DeliverySuccess" and more), "handle", "folder", "msg_type", and
        on newer phones "sender_name" and "subject"."""
    @staticmethod
    def parse(data: bytes) -> dict:
        """A bMessage from anywhere, as `get` returns it."""
    def changes(self) -> int:
        """What changed since the last call: `CONNECTION`, `MESSAGES`,
        `NOTIFY`."""
    def deinit(self) -> None:
        """Close the service and stop. The radio stays on."""
    def __enter__(self) -> Messages: ...
    def __exit__(self, *args: object) -> None:
        """`deinit`."""
