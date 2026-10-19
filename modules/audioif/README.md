# audioif

The audio hardware layer for PyDevices firmware: the platform drivers under the
audio pump. It is one C module, `_audioif`, and a CircuitPython-shaped
`audiobusio.I2SOut` on top of it — the part that knows what a thread is, what a
mutex is, where the clock comes from and where the audio goes. It sits beside
[`displayif`](https://github.com/PyDevices/displayif) and
[`usbif`](https://github.com/PyDevices/usbif), and under `audiodev` in
[pydevices](https://github.com/PyDevices/pydevices).

New here? Read the [newcomer's guide](docs/newcomers.md) for the portable
pump-to-driver boundary, normal I2S API, and firmware integration map.

The portable half — the pull loop, `service()`, the push ring, the event queue,
the tap, the status words, the fault register, `retarget`/`park` and the
finaliser guard — is the `audiopump` module in
[audiodsp](https://github.com/PyDevices/audiodsp), and ships with it on every
port. This repository binds to it by defining `audiodsp_port_driver()`.

Ask a firmware whether this half is in it:

```python
>>> import audiopump
>>> audiopump.driver()
'esp32'          # or 'pthread', 'win32', 'none'
```

`'none'` means no driver bound. That is a real answer — the engine's default
hooks are one thread and no hardware, which is how the WebAssembly build has
always run — but a build that should say `'esp32'` and says `'none'` has a
link problem, and now it says so out loud.

## `audiobusio.I2SOut` — the public face

```python
import audiobusio, audiocore, board

out = audiobusio.I2SOut(board.GPIO12, board.GPIO13, board.GPIO14)
out.play(sample, loop=True)
```

That is a CircuitPython program and it runs on this firmware unchanged, with
the pump as the engine under it. CircuitPython 10.3.0's own `I2SOut` docstring
example runs **character for character apart from the three pins**.

A second `I2SOut` raises CircuitPython's own `"Peripheral in use"`. `play()`
while playing stops the first. `pause`/`resume` are park and unpark.
`playing` goes False by itself when a sample ends. The pump loops a
`RawSample` because in CircuitPython the output does. And a `WaveFile` plays —
the pump pulls a ring the interpreter fills from a scheduler node, because a
file read cannot happen on the pump's thread.

Where it differs from CircuitPython, deliberately: `pause()` leaves the bus
clocking zeros rather than disabling the channel, and `left_justified` and
`external_clock` raise `NotImplementedError` rather than half-shifting a bus
in silence.

### The seven keywords and methods that are ours

| | what it is for |
|---|---|
| `port=` | Which I2S peripheral. CircuitPython picks one itself and so do we by default, but a board can have its codec on one port and its microphone on another, and the LilyGO T-Embed S3 does. |
| `sample_rate=` | The rate the bus is opened at, before anything plays. The first `play()` retunes it to whatever the sample asks for. |
| `main_clock_fs=` | Bit clocks of MCLK per frame. 256 is what every board in this workspace straps and what CircuitPython's espressif port hard-codes, so it is the default — but a codec wanting 384 has nowhere else to say so, and `audiodev`'s `I2SWire` has carried the number since before this class existed. Dropping it silently is a half-shifted clock that sounds like a bad cable. |
| `sink=` | Desktop only: the file the blocks are written to, which is what makes the whole lifecycle testable with no board. |
| `data_in=` | The microphone's data pin, on a board whose microphone shares this output's clocks. The channel opens as a TX/RX pair so `_audioif.rx_open()` can record while this plays (see below). The channel then stays stereo, so a mono sample is copied to both slots on the pump's thread, and while a recorder is open a sample at another rate is refused rather than retuning the bus under the recording. |
| `retarget(sample, loop=…)` | Swap the tail **without a gap** — no `stop()`, no join, no channel close and re-open between two blocks a listener is in the middle of. |
| `.status` | The pump's status `bytearray`, which is the only way a caller that spawned through `play()` can reach the two words that say why the pump stopped. |

`retarget` is here because a policy layer needs it and a program calling
`audiobusio` directly does not. `audiodev` has a new tail for the same output
every time a client arrives or leaves, and going through `play()` there is a
stop, a join and a fresh channel on every kit change and every note a second
voice starts. It is the pump's own `retarget` with the new object rooted by
this class — which is the part a caller reaching for `audiopump.retarget()`
behind its back gets wrong, because the output would still hold the old graph
and the new one would be unrooted while a thread pulls it.

Two things `retarget` refuses, each with a `ValueError` naming which:
a sample that is not signed 16-bit, and a sample at a different rate. The
conversion scratch and the feeder's ring were both sized for what `play()`
was handed and neither can be resized under a running pump, so a tail needing
either is a `play()`, not a swap. `loop` travels **with** the tail rather than
staying as `play()` set it: a root Mixer replaced by the one client still
sounding is a different thing to loop, and leaving the flag behind is exactly
how a looping client left alone on a live pump stopped at the end of its
first lap.

### What it did on silicon

On the **Waveshare ESP32-P4 panel**, at an earlier commit of this branch,
`audiobusio.I2SOut` passed **23 of 24 checks**: the docstring example, a
second output refused, a Mixer, synthio, pause and resume, 50 × play and stop,
ten soft resets while playing all clean, and a WAV off flash ending by itself
in 491 ms for a 500 ms file.

The one failure was real and is fixed. A sample at 8 kHz starved 67 blocks in
two seconds — and it was not the conversion: a signed-16 *stereo* sample at
8 kHz, converted by nothing at all, starved the same 67. It was a **240-frame
DMA descriptor at 8 kHz**, which is 16 ms of descriptor there against 2.7 ms
at 48 kHz. Sized in time (`rate / 200`) it is zero at every rate, and 48 kHz
keeps the 240 it had. The docstring example over ten seconds: **0 starved,
320 000 of 320 000 bytes of DMA**, against 334 and 319 680 before.

**`retarget()`, `.status` and `main_clock_fs=` have never run on a board.**
They compile and link for an ESP32-P4 and an ESP32-S3 — the driver's hook
table wins in both link maps and the three entry points are in both images
and not discarded — and the desktop probe covers all 39 legs. No image
carrying them has been flashed.

**`audioi2sin.I2SIn` is not built.** CircuitPython 10.3.0 keeps I2S capture
there rather than in `audiobusio`. `_audioif.Input` below is the same thing
under a private name.

**The mono path is not written.** A board with one transducer fed by a
two-slot wire gets the same signal in both slots and has no answer here.

## `_audioif` — the module this half publishes

Private by convention, the way `usbif` ships a C `_usbif` under a Python
package; `audiodev` is the public face. It carries the hardware and nothing
else. The engine's counters — `lock_stats`, `lock_reset`, `fault` — stayed on
`audiopump`, because they are the engine's.

```python
import audiopump, _audioif

cushion = _audioif.i2s_start(port, bclk, ws, dout, 48000,
                             bits=16, channels=2, mclk=13, mclk_fs=256,
                             dma_desc=4, dma_frame=128)
audiopump.spawn(fx.output, 0x7FFFFFFF, status, sink=True)
...
audiopump.shutdown()          # closes the channel with the task
```

| call | what it does |
|---|---|
| `i2s_start(port, bclk, ws, dout, rate, …)` | Opens a TX channel the pull loop writes every block into, and returns the bytes the DMA holds when full — the block-to-wire latency floor. `bits`, `channels`, `mclk`, `mclk_fs`, `dma_desc`, `dma_frame` are keywords. |
| `i2s_start(…, din=PIN)` | Opens the RX half of the **same** channel pair, so one clock tree drives capture and playback and the two DMAs cannot drift. |
| `i2s_start(…, din=, in_port=, in_bclk=, in_ws=, in_mclk=)` | Opens RX on a **different** peripheral, for a board whose microphone is not on the speaker's port. See the warning below. |
| `i2s_stop()` | Closes both channels, a recording included. |
| `rx_open(port, bclk, ws, dout, din, rate, mclk=, mclk_fs=, ring=, take=)` | Records from the RX half while an output plays on the same port. Opens the channel pair if nothing has (TX then clocks zeros until an output joins), or joins the one `I2SOut(..., data_in=)` opened; the output and the recorder each hold the channel and it closes when the last lets go. The RX interrupt copies every DMA block into a ring of `ring` bytes, taking the left slot, the right, their average or both (`take` 0, 1, 2, 3). A recorder at another rate than the open channel is refused by name. |
| `rx_read(buf)` | Copies whole frames from that ring into `buf` and returns the bytes copied, 0 when nothing is waiting. Never blocks. |
| `rx_close()` | Stops recording. The channel closes only if no output holds it. |
| `rx_stats()` | `(frames captured, frames dropped, bytes waiting)`. Dropped frames are a reader that fell further behind than `ring`. |
| `i2s_state()` | `None` with no channel open, else `(rate, channels, has_rx, holders)`, holders 1 the output, 2 the recorder, 3 both. A board asks before touching a clock pin the channel may be driving. |
| `i2s_dma_bytes()`, `i2s_rx_bytes()` | What the two DMAs actually clocked, from their ISRs. Bytes the DMA sent that the pump never wrote *are* the silence a listener heard, so these are how starvation gets measured at all. |
| `Input(sample_rate=, channel_count=, frames=, timeout_ms=)` | An audiosample whose `get_buffer` is an I2S read, so a live microphone is a source like any other and `audioeffects.create(name, Input(…), rate)` builds a graph on it. The read blocks, and that is the pacing. `stats()` reports what it has read and what it has missed. |
| `rt_probe(capture, frames=, lead=, prime=, level=, channels=, timeout_ms=)` | Round-trip latency in frames rather than in host time: it preloads a click into the TX DMA *before* either channel is enabled and captures RX from the same instant. Runs with no pump spawned — two owners of one channel is the failure that sounds like silence. |
| `driver()` | The same name `audiopump.driver()` returns. |

**Two ports are not sample-locked.** `din=` alone opens one channel pair, and
there BCLK and WS are generated once, so capture and playback cannot drift.
`in_port=` on a different peripheral is two clock dividers off one PLL: the
same nominal rate, nothing locking them frame to frame. Measured on the LilyGO
T-Embed S3, whose MAX98357A is on port 1 and whose ES7210 microphones are on
port 0, `rx − tx` stayed at **0 bytes** across forty seconds at a 4 × 128 ring
(0–512 bytes at 2 × 128). That is the honest claim: no drift at that
timescale, not proof of indefinite lock. An hour-long run is what would settle
it and it has not been done.

## What each port implements

| | thread | mutex | clock | sink | microphone |
|---|---|---|---|---|---|
| **ESP32 family** | a FreeRTOS task pinned to the core the interpreter is not on, optionally with its stack in PSRAM | `xSemaphoreCreateRecursiveMutex`, which is priority-inheriting | `esp_timer_get_time` | the I2S channel, written from the pull loop | `Input`, single-port or two-port |
| **unix** | `pthread` | recursive `pthread_mutex` | `clock_gettime` | a file the loop `write(2)`s each block to | — |
| **Windows** | `_beginthreadex` | `CRITICAL_SECTION` | `QueryPerformanceCounter` | a file descriptor | — |
| **WebAssembly** | none — **and none is needed** | — | — | — | — |

WebAssembly is not a gap. That build has no threads (no `-pthread`, no
`SharedArrayBuffer`) and no hardware to drive, and Emscripten's
`pthread_create` *links* and then aborts the whole module at run time with
"Tried to spawn a new thread, but this is not supported" — which would be the
same compiles-links-silently-wrong trap in a new place. So this file compiles
to nothing there, no driver binds, the engine's default hooks take over, and
`audiopump.driver()` says `'none'`, which is exactly true.

Two more things the desktop ports are honest about. There is no audio device
on either: the sink is a file, and what plays it is `audiodev`'s own
transport. And Windows has no `_thread` module at all, so the pump being a
native C thread there is not an optimisation — it is the only way a second
thread exists in that interpreter.

## Building it into a firmware

It is a user C module that sits beside the engine, so a firmware that has it
also has [audiodsp](https://github.com/PyDevices/audiodsp). On MicroPython 1.29
or later, clone the two side by side and include both manifests in the
manifest your build already uses:

```python
include("/path/to/audiodsp/manifest.py")
include("/path/to/audioif/manifest.py")
```

On unix that manifest is `ports/unix/variants/standard/manifest.py`; on esp32
it is usually `ports/esp32/boards/manifest.py`, unless your board brings its
own. Build as usual. audiodsp's README says how to fetch its `ulab` and `mp3`
dependencies first. This repository's manifest freezes no Python; it only names
the C module, which gives you `audiobusio` and `_audioif`. The driver finds the
engine's headers among the modules your build names, so audiodsp can be checked
out anywhere; failing that it looks at `../audiodsp`. A build that names audioif
but can't find audiodsp stops with an error saying so. Tested on the unix port against MicroPython v1.29.0, where
`audiopump.driver()` answers `'pthread'`.

The platform driver is unix, Windows and esp32. Any other CMake port (rp2, say)
skips it by design.

If you would rather not edit the MicroPython tree, write a manifest of your own
and pass it as `FROZEN_MANIFEST=`. That replaces the port's default, so include
the default too (`include("$(PORT_DIR)/variants/standard/manifest.py")` on
unix, `include("$(PORT_DIR)/boards/manifest.py")` on esp32) or you lose
`asyncio` and the port's other frozen modules.

**Older than 1.29?** Manifests there have no `c_module()`; use
`USER_C_MODULES` with both repositories. On esp32 that is
`USER_C_MODULES="/path/to/audiodsp;/path/to/audioif"`. On a Make port it is
the directory that contains both, which builds every module in it.
`AUDIODSP_DIR` (Make) and `AUDIOPUMP_AUDIODSP_DIR` (CMake) override where the
headers are looked for.

[micropython-pydevices](https://github.com/PyDevices/micropython-pydevices)'
`build_mp.py` builds the pairing on any port, with upstream's own boards:

```bash
micropython-pydevices/build_mp.py --port unix --variant pydevices --modules audiodsp,audioif
micropython-pydevices/build_mp.py --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI \
    --flash 16MB --modules audiodsp,audioif
```

Without this repository the firmware still builds and still plays: the engine's
hooks default to one thread and no hardware, `audiopump.driver()` says
`'none'`, and `audiodev` falls back to `machine.I2S` and says so once.

**`micropython.cmake` defines `AUDIOIF_DRIVER_ESP32` itself**, and that is not
belt-and-braces. The IDF does not hand `ESP_PLATFORM` to a user C module, and
the POSIX branch of this file compiles *and links* on esp32 because the IDF's
newlib has `pthread.h` — so keying off the wrong macro gets you an unpinned
pump on a default stack with no sink and nothing failing to say so. That cost
the spike a whole firmware once, and it is most of the reason
`audiopump.driver()` exists.

## Adding a port

RP2 is the obvious next one, and there is nothing to design: the engine's
`src/shared/audiodsp_port.h` is the whole interface, every one of its sixteen
hooks may be NULL, and a table of nothing but NULLs is a complete one-thread
port. Fill in `thread_start` and the mutex first — those are what turn
`service()` into a pump that runs by itself — then the sink, then `park_spin`.
Leave that last one out and you get a pump that free-runs and drops blocks
when the output ring fills instead of waiting for room; it is the one missing
hook Python can see, as `audiopump.backpressure()` returning False.

The page for it is the engine's own `docs/pump-ports.md`: what each hook must
guarantee, how the weak-symbol binding works and the static-archive trap it
dodges, and the portability gate that keeps platform includes out of the
engine. The one rule to carry away before reading it is the binding rule:
**put the hook table in the same C file as `MP_REGISTER_MODULE`**, or a static
archive can let the engine's weak default win in silence.

A new port belongs in this file, in a fifth branch beside the four above. The
branches are whole rather than a few `#ifdef`s inside one another on purpose —
nothing in the POSIX branch survives on Windows, and mixing them is how the
esp32 trap happened.

## Where this fits

This repository was the private `audiopump` driver repo until 2026-09-21. It
took the name `audioif` when the DSP repository that used to carry it became
[audiodsp](https://github.com/PyDevices/audiodsp) — so a link to
`PyDevices/audioif` written before that date was about the DSP nodes, and wants
`audiodsp` now.

What the split cost: nothing that plays. The Python module `audiopump` kept
its name and its whole surface and moved to audiodsp; `i2s_start`, `i2s_stop`,
`i2s_dma_bytes`, `i2s_rx_bytes`, `Input` and `rt_probe` moved from
`audiopump.*` to `_audioif.*`; `lock_stats`, `lock_reset` and `fault` stayed
on `audiopump`, because they are the engine's counters and not this half's.
The lock's three platform branches are gone — it calls hooks now. Every
digest in the merge's table matched afterwards, on unix, on Windows and on
WebAssembly.

## What it is not

One global pump, and nothing staged. The tail is rooted but not the graph
behind it, so the Python side has to keep holding the components. `deinit` has
no safety around it beyond the engine's lock — build first, re-point, release
last.

The soft-reset hole is closed and worth knowing about, because it is the one
piece of platform trickery that is not behind a hook. The esp32 port has no
soft-reset callback a user C module can register in: `soft_reset_exit` calls a
fixed list of `_deinit()`s and this port has no `MICROPY_BOARD_END_SOFT_RESET`.
But two lines above that list it calls `gc_sweep_all()`, which runs `__del__`
on **every** object that has one, reachable or not. So the engine allocates
one `Guard` with a finaliser, roots it so an ordinary collection never touches
it, and lets the soft reset finalise it — which reaches `teardown` here and
closes the channel. No port patch, and Ctrl-D always leaves the board quiet.

What's planned next is in [ROADMAP.md](ROADMAP.md).
