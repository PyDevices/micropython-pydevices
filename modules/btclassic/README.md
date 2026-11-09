# btclassic — Bluetooth Classic: a speaker and a hands-free unit

`import btclassic` gives a board Bluetooth Classic. Your phone or computer
lists the board under its hostname, pairs with it without a PIN, and plays to
it. Your program reads the decoded audio into any PCM output, shows everything
the phone says about what's playing, cover art included, and drives the
player. It can also follow the phone itself, as a car kit does: battery,
signal, carrier and calls, which it can answer and end, with the call's audio
both ways.

```python
import board_peripherals
import btclassic
import network
from audiodev import AudioFormat

network.WLAN(network.STA_IF).active(False)  # the radio is Bluetooth's now
bt = btclassic.Bluetooth()                  # listed as network.hostname()
music = btclassic.AudioSink(bt)
speaker = board_peripherals.pcm_out(AudioFormat(44100, 2, 16))
buf = bytearray(512 * 4)                    # 512 stereo frames
while True:
    if speaker.space() >= len(buf):         # the speaker sets the pace
        music.readinto(buf)                 # silence until audio arrives
        speaker.write(buf)
    if music.changes() & btclassic.TRACK and music.title:
        print(music.title, "-", music.artist)
```

Pair a phone or computer and play, and the board prints the title and
artist each time the track changes. Playing a tagged test file from a
Windows PC:

```text
Bench Tone A - Test Signals
```

Phones send 44.1 kHz almost always. A program that should follow any rate
opens its output at `music.sample_rate` once the `FORMAT` bit comes, as
[`examples/btclassic_simpletest.py`](examples/btclassic_simpletest.py) does.

## The radio and its profiles

The module is shaped like `bluetooth.BLE`: one radio object, and an object
per profile that takes it.

| Object | What it does |
|---|---|
| `Bluetooth()` | The radio. `active()` turns it on and off; `config()` reads and sets `name`, `discoverable`, `connectable`, `pin`, `mac`, the channels it hops on and what a search sees; `paired()` and `unpair()` manage pairings; `inquiry()` finds devices, `services()` asks one what it offers, and `rssi()` reads a link's signal. |
| `AudioSink(bt)` | Receives audio (A2DP sink) and talks to the sender's player (AVRCP): `readinto()`, the track, `cover`, `position`, `play()`, `next()`, `seek()`, the `volume` shared with the sender's slider, and more. |
| `HandsFree(bt)` | Follows a phone (HFP hands-free): `battery`, `signal`, `operator`, the `call` and its `caller`, `answer()`, `hangup()`, `dial()`, and the call's audio through its own `readinto()` and `write()`. |
| `AudioSource(bt)` | Sends audio to headphones, a speaker or a car stereo (A2DP source) and acts as their remote's target (AVRCP): `write()`, the receiver's keys through `key()`, the `volume`, and the `track()` and `status` its display shows. `Bluetooth.inquiry()` and `found()` find the receiver. |
| `Serial(bt)` | A serial port (SPP), as a stream like `machine.UART`: a computer gets a Bluetooth COM port to the board, or `connect()` calls another device's port. |
| `HIDDevice(bt)` | The board as a keyboard, mouse or game controller (HID device): `type()`, `keyboard()`, `mouse()`, `gamepad()`, or `send()` with your own report descriptor, and the keyboard `leds` the computer sends back. |
| `HIDHost(bt)` | The board reads a keyboard, mouse or controller (HID host): `connect()`, `read()` its reports, `decode()` them, and `send()` its output reports. |
| `PhoneBook(bt)` | Reads a phone's contacts and call history (PBAP client): `pull()` a book as records of names, numbers and emails, `list()` and `entry()` to page through it, `size()`, and `lookup()` for the name behind a caller's number. |
| `AudioGateway(bt)` | The board as the phone for a headset or car kit (HFP audio gateway): the `signal`, `battery` and `operator` it shows, `ring()` with a caller's number, `dial()`, the headset's answer and hang-up, and the call's audio through `readinto()` and `write()`. |
| `Messages(bt)` | Reads and sends a phone's texts (MAP client): `list()` a folder, `get()` a message with its sender and text, `mark_read()`, `delete()`, `send()`, and `notify()` to hear about new messages as they arrive. |
| `Library(bt)` | Browses the media library of the phone `AudioSink` plays from (AVRCP browsing): `players()`, `browse()` one, `items()` in a folder, the queue or a search, `enter()` and `up()`, and `play()` a track. Pico W only. |

Nothing calls back into Python. Each object's state is properties, and its
`changes()` returns a bit for each kind of state that changed since you last
asked, so a program polls it as often as it redraws. A profile closes with
`deinit()` and works as a context manager.

Everything keeps running across a soft reset: making the objects again picks
up the running ones, links and all.

A board is a sink or a source, one at a time: both are in the firmware, but
the Bluetooth stack runs one A2DP role at once. `deinit()` the `AudioSink`
before making an `AudioSource`, and the other way round.

When `AudioSink` connects to a phone, `HandsFree` follows it there. The
phone decides when a call's audio comes to the board: for a call while the
board is its audio device, and for a voice or video chat app. While
`HandsFree.audio` is True, play `HandsFree.readinto()` instead of the music
(at `HandsFree.sample_rate`, 8 or 16 kHz) and send your microphone with
`HandsFree.write()`. The type stub, [`btclassic.pyi`](btclassic.pyi), has
every property and method.

## Let the audio pump play it

In a firmware with audiodsp, `AudioSink` and `HandsFree` are audiosamples,
like a synth or an MP3 decoder. Hand one to a board's `audio_out` and the
audio pump pulls it in C on its own task: a garbage collection, a cover
decoding or a slow redraw in your program can't make the music stutter.

```python
music = btclassic.AudioSink(bt)
while not music.sample_rate:                # until the first stream
    time.sleep_ms(100)
speaker = board_peripherals.audio_out(AudioFormat(music.sample_rate, 2, 16))
speaker.play(music)                         # nothing to do in the loop now
```

It goes into a `Mixer` beside anything else at the same rate, too. For a
call, play the `HandsFree` once `audio` is True, at its `sample_rate`, and
hand `HandsFree.uplink()` the board's open `pcm_in`: the pump's pull then
sends the microphone to the phone as well. A profile is read one way or the
other, by the pump or with `readinto()`, not both.
[`examples/btclassic_speaker.py`](examples/btclassic_speaker.py) plays music
this way, and
[`examples/btclassic_speakerphone.py`](examples/btclassic_speakerphone.py)
answers calls and plays them, with the microphone going back to the phone.

If the phone's caller hears your microphone break up while the board is busy
(redrawing, decoding a cover, collecting garbage), raise the PSRAM and flash
clock to 80 MHz. On a classic ESP32, PSRAM and flash share one bus, and the
Bluetooth stack on the other core waits on it: at the default 40 MHz a busy
program holds it long enough to lose call audio. On an ESP32 Audio Kit
(ESP32-A1S module) with the display redrawing through a call, 15 s of
microphone tone had 8 or 9 dropouts at 40 MHz, and none in 13 calls at
80 MHz. If your module's PSRAM is rated for 80 MHz, add these to the board's
sdkconfig, and write the bootloader as well as the app, because the flash
clock is set in the bootloader:

```text
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_SPIRAM_SPEED_80M=y
```

On the unix port the module builds on a loopback backend with no radio, so
these paths have desktop tests: [`tests/host_probe.py`](tests/host_probe.py)
plays music and a call through the pump while the interpreter collects
garbage and stalls.

## Before you start

Wi-Fi and Bluetooth share the ESP32's one radio. With Wi-Fi connected, audio
arrives slower than it plays and in bursts, so the speaker runs dry; turn
Wi-Fi off while listening, as the examples do.

On ESP32, it's the classic ESP32: the chip in that family with Bluetooth
Classic and MicroPython support. The Pico W and Pico 2 W are covered below.
The module is opt-in (`--modules btclassic`, or `--modules all,btclassic`)
because it turns ESP-IDF's Bluedroid on in place of NimBLE. ESP-IDF runs one
Bluetooth host, so MicroPython's `bluetooth` (BLE) module runs on Bluedroid
too, beside Classic in the same firmware, with a few differences
([BLE on Bluedroid](#ble-on-bluedroid)). A board with PSRAM is the
comfortable fit: the Bluetooth stack's buffers go there.

Pairings live in the board's NVS partition, and the last sender with them.
Writing a whole `firmware.bin` at 0x1000 rewrites that partition, so every
device has to pair again (forget the board on the phone first). To update a
board whose partition table hasn't changed, write only the app,
`micropython.bin`, at 0x10000: pairings and files stay.

## On the Pico W and Pico 2 W

The rp2 port builds btclassic on BTstack, the Bluetooth host its `bluetooth`
module already uses, so a Pico firmware keeps BLE alongside Classic. Classic
adds this much to a firmware, measured against the same board built without
it:

| Board | Flash | Static RAM |
|---|---|---|
| Pico 2 W (`RPI_PICO2_W`) | +140 KB, 1037 KB in all (66% of 1.5 MB) | +41 KB, 106 KB of 512 KB |
| Pico W (`RPI_PICO_W`) | +144 KB, 1062 KB in all (86% of 1.2 MB) | +41 KB, 108 KB of 256 KB |

The big buffers come from the Python heap when you open a profile, not from
static RAM, and go back when you close it:

| Opened | Heap |
|---|---|
| `AudioSink` or `AudioSource` | 48 KB at 250 ms, 96 KB at 500 ms |
| `HandsFree` | 32 KB |
| Serial port (SPP) | 6 KB |
| `PhoneBook` | Only its replies: about 150 bytes a contact |

`buffer_ms` defaults to 500 on a board with PSRAM and 250 otherwise. Both
boards fit any one profile; on a Pico W, a sink and hands-free open together
leave little heap for anything else.

Call audio is a known limit on these boards. The CYW43439's Bluetooth
firmware accepts the audio link a call needs but never completes it
([raspberrypi/pico-sdk#1461](https://github.com/raspberrypi/pico-sdk/issues/1461)),
and the phone then drops the board altogether, so `HandsFree` declines the
link and the call's audio stays on the phone. Everything else works:
`HandsFree` follows the call, its caller and the phone's signal, battery,
service and operator, and answers, dials, hangs up and sends tones.
`HandsFree.audio` stays False, and `connect_audio()` raises OSError.

`PhoneBook` works as it does on the ESP32, except that `new_missed` is
always None. Its reply comes into the heap and grows with it: a phone book
of 471 contacts without photos is 68 KB, so page with `count` on a Pico W.
`AudioGateway` and `Messages` are ESP32-only. `Library` is the other way
round: only the Pico W has it (see [A phone's music library](#a-phones-music-library)).

## How it's built

[`esp-idf/btclassic`](esp-idf/btclassic) is the C that does the work, with
no MicroPython in it, one file per part: the radio, the A2DP sink, the AVRCP
controller with cover art, the HFP hands-free unit with call audio over HCI
(Bluedroid codes CVSD and mSBC), and the audio ring they play through. They
sit behind one header, [`btclassic.h`](esp-idf/btclassic/include/btclassic.h),
which another Bluetooth host could implement as well.
[`src/mod_btclassic.c`](src/mod_btclassic.c) wraps that header, and makes
the two profiles audiosamples when audiodsp is in the build.
[`micropython.cmake`](micropython.cmake) compiles both as an
ordinary user C module, so a stock MicroPython esp32 build takes it with
`USER_C_MODULES`. Two settings go with it: [`sdkconfig.esp32`](sdkconfig.esp32)
in the board's `SDKCONFIG_DEFAULTS` turns Bluedroid, A2DP, AVRCP cover art, HID and
HFP on, and `micropython.cmake` puts the `bluetooth` module on Bluedroid
([`src/modbluetooth_bluedroid.c`](src/modbluetooth_bluedroid.c)), with two
small MicroPython patches in [`patches/`](patches). `build_mp.py` does the
first for you.

## BLE on Bluedroid

`import bluetooth` works as upstream documents it: advertising, scanning,
both roles, the GATT server and client, and pairing. Where it differs:

- `_IRQ_GATTS_READ_REQUEST` can't refuse a read. Events reach Python through
  the scheduler, never on the Bluetooth task, which also decodes audio.
- Bonds live in NVS, kept by Bluedroid, so `_IRQ_GET_SECRET` and
  `_IRQ_SET_SECRET` never fire.
- The client hears notifications from characteristics it discovered (up to
  16 per peer), or whose CCCD it wrote.
- The device name is one name, shared with Classic.

## Sending audio

```python
import btclassic

bt = btclassic.Bluetooth()
bt.inquiry(10240)                           # headphones in pairing mode
# ... wait for bt.found(), pick an address ...
source = btclassic.AudioSource(bt, rate=16000, channels=1)
source.connect(address)
source.track("Test tone", "btclassic")      # what the receiver shows
while True:
    source.write(tone)                      # waits while the stream runs
```

`write()` takes 16-bit PCM at the `rate` and `channels` you choose, and the
source converts it to the stream's: 44.1 kHz stereo SBC, the one format
every A2DP receiver takes. While the stream runs, `write()` waits for room,
so the receiver's pace sets your program's.
[`examples/btclassic_source_tone.py`](examples/btclassic_source_tone.py)
plays a tone and prints the receiver's keys, and
[`examples/btclassic_transmitter.py`](examples/btclassic_transmitter.py)
sends the board's line in.

The receiver's display gets the title, artist, album, play status and
position through AVRCP. ESP-IDF's Bluetooth stack has no public call for
that, so btclassic answers those requests itself, at link time, on the
ESP-IDF versions it was checked against (5.5 and 6.1). On any other, the
source still plays; `link()["metadata"]` says whether this firmware sends
it. Builds can turn it off with `-DBTCLASSIC_TG_METADATA=OFF`.

C code can feed the source without Python: `btc_source_write()` in
[`btclassic.h`](esp-idf/btclassic/include/btclassic.h) takes PCM from any
one task, at any rate, and returns how much it took.

The same folder is also an ESP-IDF component of its own. CI builds it in a
plain ESP-IDF 6.1 app for the ESP32-S31, which has Bluetooth Classic too, so
it is ready for that chip once MicroPython supports it.

## Keyboards, mice and game controllers

```python
import btclassic

bt = btclassic.Bluetooth()
hid = btclassic.HIDDevice(bt, btclassic.HIDDevice.COMBO)
# ... pair from your computer, wait for hid.connected ...
hid.type("Hello\n")                         # US layout
hid.mouse(40, -20)                          # move the pointer
```

`HIDDevice` comes with four report descriptors: `KEYBOARD`, `MOUSE`,
`COMBO` (both, plus media keys through `consumer()`) and `GAMEPAD`. Pass any
other descriptor as bytes and send its reports with `send()`, report ID
first when the descriptor has IDs. The computer's keyboard LEDs come back
in `leds`, and `changes()` sets `btclassic.HID` when they do.
[`examples/btclassic_hid_keyboard.py`](examples/btclassic_hid_keyboard.py)
types a line and moves the pointer.

`HIDHost` is the other end: `connect(address)` opens a keyboard, mouse or
controller, `descriptor` holds its report descriptor, and `read()` returns
each input report. `decode()` turns a report into a tuple such as
`("keyboard", modifiers, keys)` when the device uses the boot layout
(`connect(address, boot=True)`) or is another btclassic board. A board is a
device or a host, one at a time.

[`examples/btclassic_hid_host.py`](examples/btclassic_hid_host.py) reads
whatever connects, and two more devices give it something to read:
[`examples/btclassic_hid_gamepad.py`](examples/btclassic_hid_gamepad.py)
turns a gamepad's stick and keeps a feature report the host reads and
changes, and
[`examples/btclassic_hid_media_keys.py`](examples/btclassic_hid_media_keys.py)
steps a phone's volume.

A computer learns what the board offers when it pairs. If it paired with
the board as a speaker, or as a different kind of HID device, forget the
board on the computer, `unpair()` the computer on the board, and pair
again: until then it won't connect.

## Contacts and calls

```python
import btclassic

bt = btclassic.Bluetooth()
pb = btclassic.PhoneBook(bt)
pb.connect()                                # the phone HandsFree follows, or the first paired one
for card in pb.pull("pb", count=5):
    print(card["name"], card["numbers"])
for card in pb.pull("mch", count=3):        # missed calls
    print(card["time"], card["name"] or card["numbers"])
print(pb.lookup("+15550100"))               # the name a call screen shows
```

The phone asks before it shares its contacts the first time; allow it
there. `pull()` gives records as dicts, and `PhoneBook.parse()` reads a
`.vcf` file the same way.

`AudioGateway` turns it round: a headset or car kit pairs with the board as
it would with a phone. `ring("+15550100")` rings it with that number, its
button answers, and while `audio` is True the call's sound goes both ways
through `write()` and `readinto()`. A board runs `HandsFree` or
`AudioGateway`, one at a time.

`Messages` reads the phone's texts the same way:

```python
import time

msgs = btclassic.Messages(bt)
msgs.connect()
for m in msgs.list("inbox", count=5):
    print(m["datetime"], m["sender_name"], m["subject"])
msgs.notify()                               # the phone reports new messages
while True:
    event = msgs.event()
    if event and event["type"] == "NewMessage":
        print(msgs.get(event["handle"])["text"])
    time.sleep_ms(100)
```

`send(number, text)` sends a text through the phone; `folder="draft"`
leaves it in the phone's drafts instead.

A moment after `notify()`, the phone connects back to deliver its reports,
and `notify_connected` turns True. If it stays False, poll
`list("inbox", unread=True)` instead.

## Signal, channels and what a search sees

`Bluetooth.rssi(addr)` reads how a connected device's signal sits against
the range the controller wants, in dB: 0 is inside it, and the further below
0, the weaker. Any profile's link counts:

```python
import btclassic

bt = btclassic.Bluetooth()
sink = btclassic.AudioSink(bt)
# ... once sink.connected ...
print(bt.rssi(sink.peer))                   # 0: inside the ideal range
```

Bluetooth hops across 79 channels, 2402 to 2480 MHz. Wi-Fi sits on 22 MHz
of the same band, so a board near a busy network can keep off it with
`config(channel_map=...)`, ten bytes with a bit per channel:

```python
wifi = 2412 + 5 * (6 - 1)                   # Wi-Fi channel 6's centre, MHz
m = bytearray(10)
for ch in range(79):
    if abs(2402 + ch - wifi) > 11:
        m[ch // 8] |= 1 << (ch % 8)
bt.config(channel_map=m)
```

`config(page_timeout=3000)` makes `connect()` to a device that's away give
up after 3 s rather than 5 to 15.

A device searching with an inquiry gets the board's name. `eir_uuids=True`
adds the services running, so it sees a speaker or a serial port before it
connects, and `eir_manufacturer` and `eir_url` carry data of your own.
`Bluetooth.eir(addr)` reads what another device sent, after `inquiry()`
found it.

`Bluetooth.services(addr)` asks a device what it offers, from its SDP
server, without pairing. It calls the device if no link is up, and returns
each service's class UUID:

```python
print([hex(u) for u in bt.services(laptop) if isinstance(u, int)])
# ['0x110a', '0x111e', '0x1115', '0x110b', '0x110e', '0x111f']
```

That Windows laptop sends audio (0x110A) and plays it (0x110B), sends
remote-control keys (0x110E), joins a network over Bluetooth (0x1115), and
takes either side of a hands-free call (0x111E, 0x111F). The first search pages the device, about 2 s; a second
one over the same link took 0.15 s.

## Pairing with a passkey or a number check

By default the board pairs the way most speakers do, "just works", with
nothing to show or type. A board with a screen, or a REPL someone watches,
can pair with a check instead: tell it what it has with `config(io=...)`,
and pairing asks your program through `Bluetooth.changes()` and
`passkey()`:

```python
bt.config(io=btclassic.IO_DISPLAY_YESNO)
port = btclassic.Serial(bt)
while True:
    if bt.changes() & btclassic.PASSKEY:
        addr, action, number = bt.passkey()
        if action == btclassic.PASSKEY_COMPARE:
            ok = input("Does it show %06d? [y/n] " % number) == "y"
            bt.passkey_reply(ok)
```

`IO_KEYBOARD_ONLY` gets `PASSKEY_INPUT`: type the number the other device
shows and answer with `passkey_reply(True, number)`. `IO_DISPLAY_ONLY` gets
`PASSKEY_DISPLAY`, a number to show and nothing to answer. With any of the
three, a `Serial` link asks the other side for a checked pairing too.
`examples/btclassic_pairing.py` is the whole program.

## A phone's music library

On a Pico W, `Library` browses the players on the phone `AudioSink` plays
from, and plays what you pick:

```python
import btclassic

bt = btclassic.Bluetooth()
music = btclassic.AudioSink(bt)
# ... once the phone has connected:
lib = btclassic.Library(bt)
lib.connect()
player = [p for p in lib.players() if p["browsable"]][0]
lib.browse(player["id"])
for item in lib.items(count=10):
    print("folder" if item["folder"] else "track", item["name"])
```

Folders have a `uid` to `enter()`; tracks have one to `play()`. A player
that isn't browsable may become so once its app is open on the phone.

An Android phone lists one player, "Media Player", whose folders are the
phone's media apps that let Bluetooth browse them. If none does, `browse()`
raises "that player can't be browsed", and `connect()` and `players()` are
all you get. Whether `search()` works is up to the player too: the phone
answers with an error when it can't search.

## Examples

Each runs as it is with `mpftp run`; its docstring says what it needs.

| Example | What it does |
|---|---|
| [btclassic_simpletest.py](examples/btclassic_simpletest.py) | Plays your phone through the board's speaker |
| [btclassic_speaker.py](examples/btclassic_speaker.py) | A Bluetooth speaker the audio pump plays |
| [btclassic_speakerphone.py](examples/btclassic_speakerphone.py) | A speakerphone for your phone's calls |
| [btclassic_source_tone.py](examples/btclassic_source_tone.py) | Plays a tone on Bluetooth headphones or a speaker |
| [btclassic_transmitter.py](examples/btclassic_transmitter.py) | Sends the line in to headphones or a speaker |
| [btclassic_serial_echo.py](examples/btclassic_serial_echo.py) | A serial port that echoes in upper case |
| [btclassic_hid_keyboard.py](examples/btclassic_hid_keyboard.py) | Types a line as a keyboard and moves the pointer |
| [btclassic_hid_media_keys.py](examples/btclassic_hid_media_keys.py) | Presses media keys on a phone or computer |
| [btclassic_hid_gamepad.py](examples/btclassic_hid_gamepad.py) | A gamepad, with a setting the host reads and changes |
| [btclassic_hid_host.py](examples/btclassic_hid_host.py) | Reads a keyboard, mouse or gamepad |
