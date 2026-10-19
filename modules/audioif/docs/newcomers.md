# Newcomer's guide to audioif

`audioif` is the hardware half of PyDevices audio. It supplies the platform driver beneath audiodsp's portable `audiopump`: threads, locks, clocks, and I2S output. Its normal public API is CircuitPython-shaped `audiobusio.I2SOut`; `_audioif` is the lower-level native module.

It is firmware source, not a pip package. Build it alongside `audiodsp` when a target needs an actual audio driver.

## Start with the portable API

```python
import audiobusio
import board

out = audiobusio.I2SOut(board.GPIO12, board.GPIO13, board.GPIO14)
out.play(sample, loop=True)
```

Only one output may own a peripheral, a new `play()` replaces the current sample, and `playing` becomes false when a finite sample ends. `retarget()` is a PyDevices extension for replacing a graph tail without closing the active channel.

```python
import audiopump
print(audiopump.driver())  # esp32, pthread, win32, or none
```

`none` is intentional for WebAssembly and one-thread/no-sink targets. On firmware expected to drive I2S, it means the platform driver did not link.

## The mental model

```text
sample or audiodsp graph -> audiopump -> audioif driver hooks -> I2S DMA
                            |              (thread, mutex, clock, sink)
                            `-- portable rings, events, and status
```

The portable engine belongs in [audiodsp](https://github.com/PyDevices/audiodsp). audioif defines the `audiodsp_port_driver()` hooks that give that engine a real platform. Keep hardware policy here and DSP behavior in audiodsp.

## Repository map

| Path | Purpose |
|---|---|
| `_audioif.c` | Platform driver hooks and native lower-level API. |
| `audiobusio.c` | CircuitPython-compatible `I2SOut` implementation. |
| `audiopump_i2s.h` | I2S driver interface shared by native pieces. |
| `micropython.mk`, `micropython.cmake` | Make/CMake user-module integration. |
| `manifest.py` | Native module declaration for combined manifests. |
| `tests/` | Desktop probes (`run_probes.sh`) plus S3 drift and stall witnesses. |

## Building and target boundaries

Clone `audioif` beside `audiodsp`. Both must be included as user modules; each build file finds audiodsp headers through a sibling path by default. `micropython-pydevices` supplies ready-made audio manifests and board variants, while the root README gives upstream build commands.

ESP32 has the production I2S driver and can expose microphone capture through `_audioif.Input`. Unix and Windows use native threads with a file sink for deterministic testing. WebAssembly deliberately binds no driver because it has neither threads nor a hardware sink.

`I2SOut` accepts PyDevices-specific `port=`, `sample_rate=`, and `main_clock_fs=` options for board wiring. Its desktop-only `sink=` option is for testing. Do not use `retarget()` to change sample rate or signed-16 format: the running DMA and pump buffers are sized for the original stream.

## Contributor boundary

This code is timing- and lifecycle-sensitive. A driver hook table belongs in the same C file as module registration so static linking cannot silently select audiodsp's weak default hooks. Read [the pump-port guide](https://github.com/PyDevices/audiodsp/blob/main/docs/pump-ports.md) before adding a platform.

Start with an existing desktop probe or a narrowly scoped diagnostics change. Hardware work needs real target validation; a successful compile alone does not prove a bound driver, DMA progress, or safe soft reset.

