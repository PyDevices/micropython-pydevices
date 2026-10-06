# Newcomer's guide to micropython-pydevices

This repository builds MicroPython firmware with PyDevices modules in it. It
holds our patches to one pinned MicroPython release, our variants, and the
build command. It is not a fork, and it publishes no Python package.

## Build something

```bash
./build_mp.py --port unix --variant pydevices --modules pydevices,displayif,pygraphics
./build_mp.py --port esp32 --board ESP32_GENERIC_S3 --variant SPIRAM_OCT --flash 8MB --modules all
./build_mp.py          # asks for port, board, variant, flash size and modules
```

The first run fetches what it needs. In the PyDevices workspace it links the
sibling checkouts instead.

The desktop `pydevices` variants freeze a board config that imports the
pydevices package, so name `pydevices` (or use `all`) with them; without it
`import board_config` fails.

## Boards we build for

Every board here is one of upstream's, with at most a small variant of ours
on top. Add `--modules` to any of these:

| Board | Command |
|---|---|
| Waveshare ESP32-P4-WIFI6-DEV-KIT | `--port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI --flash 16MB` |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-4B | `--port esp32 --board ESP32_GENERIC_P4 --variant PRE_REV3_C6_WIFI --flash 32MB` |
| Waveshare ESP32-S3-Touch-LCD-7 | `--port esp32 --board ESP32_GENERIC_S3 --variant LCD_7 --flash 8MB` |
| Waveshare ESP32-S3-Touch-LCD-4.3 | `--port esp32 --board ESP32_GENERIC_S3 --variant SPIRAM_OCT --flash 8MB` |
| LilyGO T-Embed S3 | `--port esp32 --board ESP32_GENERIC_S3 --variant T_EMBED --flash 16MB` |
| WT32-SC01 Plus | `--port esp32 --board ESP32_GENERIC_S3 --flash 16MB` |

A new board usually needs no file at all: pick the upstream board and
variant that match its chip and PSRAM, and give `--flash` its flash size. Make
a variant of ours only for settings that really are that board's, like the
LCD-7's cache sizes; see [esp32 settings](#esp32-settings) below.

## The model

```text
pinned MicroPython tag (UPSTREAM)
   + patches/micropython/ and each module's own patches, one local commit
             |
   upstream board (esp32, rp2)    or    upstream / our variant (unix, windows, webassembly)
   + on esp32: our sdkconfig, appended last (flash size, grown partition table, fragments)
             |
   modules/manifest.py: the port's own content, then the modules you named
             |
   the port's own make  ->  builds/<port>/[<board>/]<variant>/
```

Three choices make a build: the board (or not, on the desktop ports), the
variant (always optional, as with upstream's `make`), and the modules. A
module is a directory with a `manifest.py`, or a C module with its glue,
like ulab. A module never brings another; nothing checks dependencies, so
name ulab when you name audiocomponents, or use `all`.

On esp32 the build may grow the app partition to fit the image. That moves
the filesystem, so a board flashed with that image comes up with an empty
one. `--no-autosize` refuses instead.

## esp32 settings

On esp32, `build_mp.py` appends our ESP-IDF settings after the board's, so
ours win. They come from files it finds by name, each used only if it exists,
in this order:

1. `variants/esp32/sdkconfig`, then `variants/esp32/sdkconfig.<chip>`
   (`esp32s3`, `esp32p4`): what every build, or every build for that chip,
   wants. The S3's PSRAM, Wi-Fi and lwIP tuning and the P4's BLE-over-C6
   settings are here.
2. The same two names at the root of each module you selected: what that
   module needs. usbif's USB host settings and cameraif's OV5647 driver live
   in their own repos.
3. `variants/esp32/<BOARD>/<VARIANT>/sdkconfig`: what's left that is one
   board's alone. A variant dir of ours also holds an `mpconfigvariant.cmake`
   that includes the upstream variant it builds on, which is how `LCD_7`
   and `T_EMBED` become variants of `ESP32_GENERIC_S3`.

The partition table is found the same way, the most specific
`partitions.csv` winning (`variants/esp32/partitions.esp32s3.csv` adds a
coredump partition on the S3); without one, the board's own is used. Either
way autosize grows the app partition when the image doesn't fit.

## Where things are

| Path | What it is |
|---|---|
| `build_mp.py` | The build command. |
| `UPSTREAM` | The MicroPython release the patches apply to. |
| `modules.lock`, `deps.lock` | What gets fetched when it's missing. |
| `modules/` | The module manifest, `all/`, `castif/`, and the modules. |
| `patches/` | The patch series and `apply_patches.py`. |
| `variants/` | Our unix, windows and webassembly variants, and the esp32 settings and board variants. |

## Boundaries

The upstream tag is a compatibility boundary: moving `UPSTREAM` means
re-validating every patch. Never hand-edit a patched checkout; change the
patch and prepare again (`patches/apply_patches.py`).

The `vst3-engine` variants are deliberately narrow. They set
`MICROPY_PY_SOCKET`, `MICROPY_PY_SSL` and `MICROPY_PY_FFI` to 0, in the
variant's header as well as its makefile, so plugin content can't reach the
network or native libraries. A prepared tree carries the Windows networking
and FFI patches regardless; the variant is the guard. mpvst's
`mpvst_engine_capabilities` test fails if an engine can import any of them.
Don't swap it for a broader desktop variant because that one builds.

## Safe first contributions

Documentation, provenance, or a narrowly scoped variant or module fix. Check
a patch change with `patches/apply_patches.py --check` before you commit it.
The [build plan](build-plan.md) is the source of truth for the layout.
