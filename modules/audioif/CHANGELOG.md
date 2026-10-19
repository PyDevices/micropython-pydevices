# Changelog

## v0.1.2 (2026-10-10)

- **Record while you play, on one I2S port.** `_audioif.rx_open()` reads the RX
  half of the channel pair while `audiobusio.I2SOut` plays on the TX half, so a
  board whose speaker codec and microphone share a port (the ESP32-P4 boards)
  can do both at once. The output and the recorder each hold the channel and it
  closes when the last one lets go. `I2SOut` takes `data_in=` to open the pair,
  keeps the channel stereo while it can carry a recording, and refuses a sample
  at another rate while a recorder is open instead of retuning the bus under
  it. `retarget()` now also refuses a tail with another channel count, which
  used to play at twice or half speed.
  ([#25](https://github.com/PyDevices/audioif/pull/25))
- **`audiobusio.I2SOut.starved()` reads 0 through a clean playback.** It was
  the wire's position minus what the pump had fed, and that difference also
  carries how full the DMA ring is, so a clean playback on the ESP32-P4 read
  tens to thousands of bytes, a `gc.collect()` made it fall, and a real
  underrun read low by the ring's depth. It now counts two events in the DMA's
  interrupt: a descriptor started with nothing written into it (the ring ran
  dry), and a gap of more than two descriptor periods between finished
  descriptors (the DMA itself stood still, as a flash erase does on the
  ESP32-S3). Counting starts at the pump's first write after `play()` or
  `resume()`, stops at `pause()`, and only `play()` clears it, so the number
  never goes down and a pause is not counted.
  ([#26](https://github.com/PyDevices/audioif/issues/26))
- `audiobusio.I2SOut(bit_clock, word_select, data)` without `port=` opens. The
  default used to reach the driver as port -1 and fail with `EIO`; it now lets
  the driver choose the port, as on CircuitPython.
  ([#27](https://github.com/PyDevices/audioif/pull/27))

## v0.1.1 (2026-09-24)

- **`audiobusio.I2SOut.starved()` reports silence on the wire, in bytes.** It
  used to return the engine's sink-refusal count, which needs the pump running
  and the DMA full -- the one case that is not an underrun -- so it read 0
  through a stall that plainly starved the wire, on both chips. It now reports
  audio that should have played and did not, taking whichever of two witnesses
  is further on: the DMA's own byte count, which is right when the ring runs
  dry and `auto_clear` clocks zeros, and the wall clock, which is right when
  the DMA itself stops -- which is what a flash erase does on the ESP32-S3,
  measured at 1.14 seconds of audio lost from one 32 kB write with the pump
  sitting level with the DMA throughout. Neither witness alone sees both. The
  old number keeps its meaning under its real name, `sink_timeouts()`, and
  `_audioif` gains `i2s_sink_bytes()` and `i2s_starved_bytes()` so the
  subtraction is checkable from Python. Silence that was asked for -- a
  stopped or paused output -- is not charged: the count restarts wherever
  feeding legitimately begins, and arms for one ring period so the wait for
  the pump's first block reads as the latency it is.
  ([#8](https://github.com/PyDevices/audioif/issues/8))
- **`_audioif.lock_probe()`** asks FreeRTOS whether it refuses a give of the
  pump mutex from a task that does not own it, and one with nobody holding it.
  ESP32 only; nothing in the audio path calls it.
  ([#14](https://github.com/PyDevices/audioif/pull/14))
- **`__revision__`** says which audioif a firmware was built from: a
  `git describe` of this repository, or `unknown` without git.
  ([#17](https://github.com/PyDevices/audioif/pull/17))
- **One `include()` line adds this module to your own MicroPython 1.29 build.**
  `manifest.py` names the C module with `c_module()`, and the README shows the
  line. ([#19](https://github.com/PyDevices/audioif/pull/19))
- `micropython.cmake` skips the driver on CMake ports other than esp32, so an
  rp2 build that includes it no longer fails on ESP-IDF's I2S header.
  ([#15](https://github.com/PyDevices/audioif/issues/15))
- `tests/run_probes.sh` runs CI's probes against a desktop interpreter, and
  refuses one that was not built from this checkout's HEAD. CI builds against
  audiodsp v0.5.1 rather than its default branch.
  ([#6](https://github.com/PyDevices/audioif/issues/6))
- A newcomer's guide in `docs/newcomers.md`.

## v0.1.0 (2026-09-21)

- **This repository is `audioif`**, the audio hardware layer, and public. It was
  the private `audiopump` driver repo until today; the name belonged to the DSP
  repository, which is [audiodsp](https://github.com/PyDevices/audiodsp) now.
- **First public shape.** The audio pump's platform driver: the `_audioif` C
  module (`i2s_start` with a one-port and a two-port form, `i2s_stop`,
  `i2s_dma_bytes`, `i2s_rx_bytes`, `Input`, `rt_probe`) and a
  CircuitPython-shaped `audiobusio.I2SOut` (`play`, `stop`, `pause`, `resume`,
  `playing`, `paused`, `deinit`, plus `retarget()`, `.status` and `starved()`).
  Drivers for ESP32 (a pinned FreeRTOS task and the I2S peripheral), unix
  (`pthread`) and Windows (`_beginthreadex`); WebAssembly and any port without
  threads run the engine through `audiopump.service()` and need nothing from
  here.
- The portable half — the pull loop, the ring, the event queue, the tap and the
  lock as port hooks — lives in
  [audiodsp](https://github.com/PyDevices/audiodsp). This repository binds to
  it by defining `audiodsp_port_driver()`.
- Proved on an ESP32-P4 (ES8311 codec) and an ESP32-S3 (MAX98357A amplifier),
  on unix and on Windows. What is open is in the issues.
