# Newcomer's guide to micropython-pydevices

`micropython-pydevices` is PyDevices' versioned MicroPython runtime overlay. It holds the patch queue, user-module manifests, board directories, and build variants applied to one pinned upstream MicroPython release. It is not a fork and it does not publish a Python package.

## Start by preparing a checkout

`tools/prepare-micropython.sh` is the documented path, and it takes no profile. It checks out the pinned tag in a `micropython` clone beside this repository, applies the `windows-full`, `webassembly-pydevices`, `esp32-s3-debug`, `esp32-audio`, `esp32-webrepl` and `esp32-tinyusb` profiles (together, all fourteen patches) plus the usbif and cameraif module patches, and records the result as one local commit.

After that you choose what to build with paths passed to upstream's `make`:

- A `manifest` selects the sibling PyDevices repositories frozen into a build.
- A `board` supplies ESP32 board-specific sdkconfig, partitions, and defaults.
- A `variant` supplies Unix, Windows, or WebAssembly build configuration.

Profiles matter only when you call `apply.sh <profile> <micropython-dir>` directly. `apply.sh <profile> <micropython-dir> --check` tests whether that profile's patches apply cumulatively to the checkout's HEAD, in a scratch worktree, without touching the checkout. Run it against a clean checkout at the pinned tag: on a checkout prepare has already patched, it reports DOES NOT APPLY.

The root [README](../README.md) is the source of truth for these categories.

## The mental model

```text
pinned upstream MicroPython tag
             |
             v
tools/prepare-micropython.sh: all eleven patches
+ usbif and cameraif patches, one local commit
             |
     +-------+--------+
     |                |
manifest         board or variant
     |                |
     +-------+--------+
             v
       upstream build command
```

After preparation, builds use upstream's normal tools; PyDevices-specific choices are expressed by the manifest, board, and variant paths.

## Repository map

| Path | Purpose |
|---|---|
| `UPSTREAM` | Exact upstream MicroPython release the overlay applies to. |
| `patches/` | Ordered mailbox patches with individual provenance. |
| `profiles/` | Named ordered subsets of patch numbers. |
| `manifests/` | Frozen-module presets that include sibling repositories. |
| `boards/esp32/` | Out-of-tree ESP32 boards, sdkconfig, partitions, and defaults. |
| `variants/` | Out-of-tree Unix, Windows, and WebAssembly variants. |
| `usermods/wasmbridge/` | The `_wasm_bridge` C module the WebAssembly `pydevices` variant builds in: browser framebuffers, input events, timers, audio, and HTTP. |
| `tools/prepare-micropython.sh` | Pinned-checkout preparation tool. |
| `apply.sh` | Apply or verify a profile against a checkout. |
| `provenance.json` | Patch checksums and migration records. |

## Important boundaries

The upstream tag is a compatibility boundary. Moving `UPSTREAM` requires revalidating every patch and changing the overlay release identity. Do not hand-edit an already patched MicroPython checkout; change the overlay source and regenerate the affected patch.

The `vst3-engine` variants are deliberately narrow. They set `MICROPY_PY_SOCKET`, `MICROPY_PY_SSL` and `MICROPY_PY_FFI` to 0 so untrusted plugin content cannot reach the network or native libraries. That is the enforcement: a prepared checkout carries the Windows networking and FFI patches (0001, 0003) anyway, and the `vst3-engine` profile, which leaves them out, only matters when you apply it by hand. On windows the variant's `mpconfigvariant.h` has to say it too: the port header turns sockets and FFI on unless a variant has already defined them, and a make-level 0 never reaches the compiler (that is how the 0.3.0 and 0.3.1 engines kept `socket`). mpvst's `mpvst_engine_capabilities` ctest runs each engine and fails if any of them imports. Do not replace the variant with a broader desktop one merely because it builds.

A manifest names modules from sibling repositories. It controls what is built into firmware (frozen Python and C modules), not what gets installed later with MIP. Consult [the manifests guide](../manifests/README.md) before adding a repository or duplicating a preset.

## Safe first contributions

Start with documentation, provenance, or a narrowly scoped profile/manifest correction. Validate a profile with `apply.sh --check` against a clean checkout at the pinned tag before changing patches. Board and variant work should preserve the split between upstream configuration and PyDevices-owned overlay files.
