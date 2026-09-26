# micropython-pydevices

The versioned runtime overlay for MicroPython in the PyDevices project:
every downstream patch, usermod, and variant PyDevices maintains on top of
a **pinned upstream release** (`UPSTREAM`, currently v1.29.0), kept the way
a distribution keeps its patch queue — an ordered mailbox series with
provenance, applied to a clean tree, never a fork.

New here? Read the [newcomer's guide](docs/newcomers.md) for the overlay
model, preset selection, and upstream-boundary rules.

## Layout

- `UPSTREAM` — the upstream MicroPython release this series applies to.
- `patches/` — the ordered mailbox series (`0001-…` to `0014-…`): Windows
  networking/sockets/select/SSL, Windows FFI, desktop scheduler depth, the
  WebAssembly set (Asyncify, node hooks, soft reinitialization, jsffi across
  reinit, lexer EOF), the esp32s3 `SPIRAM_OCT_DEBUG` variant, esp32
  `machine.I2S` MCLK (`mck=`), esp32 WebREPL Ctrl-C in loops that
  never wait, a variant-settable default GC heap for the desktop ports,
  piped stdin for the Windows REPL (`micropython.exe -i`), and TinyUSB 0.21
  for the esp32 port.
- `profiles/` — named subsets: `windows-networked`, `windows-full`,
  `desktop-pydevices`, `webassembly-pydevices`, `esp32-s3-debug`,
  `esp32-audio`, `esp32-webrepl`, `esp32-tinyusb`, and `vst3-engine` (`*.series` = ordered
  patch numbers).
  They matter only when you call `apply.sh` yourself:
  `tools/prepare-micropython.sh` applies every patch regardless.
  `vst3-engine` leaves out the networking (0001) and FFI (0003) patches, but
  the micropython-vst3 sidecar's real guard is the `vst3-engine` variant
  below, which switches sockets, SSL and FFI off so DAW plugin content —
  compositions are code, and some of it runs at plugin-scan time — cannot
  reach the network or arbitrary native libraries.
- `manifests/` — presets: frozen manifests a build is pointed at with
  `FROZEN_MANIFEST=`, each a short list of the sibling repositories it
  carries (`kitchen-sink.py` finds every sibling with a manifest). See
  `manifests/README.md`.
- `boards/esp32/` — out-of-tree board directories (`BOARD_DIR=`): the stock
  board plus this board's partition table and sdkconfig fragments, and a
  default manifest.
- `variants/unix/`, `variants/windows/` — out-of-tree variants
  (`VARIANT_DIR=`): `pydevices` (upstream's default variant plus ours) and
  `vst3-engine` (the micropython-vst3 sidecar: no sockets, SSL or FFI).
- `tools/prepare-micropython.sh` — puts the pinned tag, the patch series
  and the module-tied patches on a MicroPython checkout beside this
  repository, once, as a local commit. After that everything above builds
  with upstream's own `make`, nothing of ours on the command line but
  these paths.
- `usermods/wasmbridge/` — the `_wasm_bridge` user C module the WebAssembly
  `pydevices` variant builds in: browser framebuffers, input, timers, audio,
  and HTTP.
- `variants/webassembly/` — the external WebAssembly variant tree
  (including the Fetch-backed `requests`).
- `provenance.json` — patch checksums and migration provenance.
- `apply.sh <profile> <micropython-dir> [--check]` — apply or verify a
  profile against a checkout.

## Rules

- Upstream is **pinned**; moving `UPSTREAM` re-validates every patch and
  bumps the overlay release id (`mp-v1.29.0-pydevices.N`, matching `UPSTREAM`).
- Patches are individually justified in their headers; no hand-edits to a
  patched tree — regenerate the patch.
- Publishing overlay *binaries* is a separate decision from this source
  repository.

Migrated from `PyDevices/cmods` (2026-08-29). cmods was archived and deleted
on 2026-09-23; the PyDevices workspace's `tools/build_interpreters.sh` now
consumes this overlay.
