# Newcomer's guide to micropython-pydevices

This repository builds MicroPython firmware with PyDevices modules in it. It
holds our patches to one pinned MicroPython release, our variants, and the
build command. It is not a fork, and it publishes no Python package.

## Build something

Get it, if you haven't: `curl -fsSL https://pydevices.github.io/install.sh | sh`
clones this repository into `micropython-pydevices` in the current directory
and checks your tools. On Windows, see [On Windows](#on-windows).
Then, from the clone:

```bash
./build_mp.py --port unix --variant pydevices --modules pydevices,displayif,pygraphics
./build_mp.py --port esp32 --board ESP32_GENERIC_S3 --variant SPIRAM_OCT --flash 8MB --modules all
./build_mp.py          # asks for port, board, variant, flash size and modules
```

The first run fetches what it needs. Where the repositories are already
checked out side by side, it links those sibling checkouts instead.

The desktop `pydevices` variants freeze a board config that imports
`displaydev`, which is frozen only when `--modules` names `pydevices` (the
repository whose `lib/` holds it) or `all`; without it `import board_config`
fails.

## On Windows

You can build the windows port natively, from PowerShell, with MSYS2's
MinGW toolchain. You need Python from python.org and Git for Windows; the
rest is a one-time install of MSYS2 and four of its packages:

```powershell
winget install --id MSYS2.MSYS2 -e
C:\msys64\usr\bin\bash.exe -lc "pacman -Syu --noconfirm"
C:\msys64\usr\bin\bash.exe -lc "pacman -S --needed --noconfirm make mingw-w64-x86_64-gcc autoconf automake libtool"
```

If the first `pacman` line says it updated MSYS2's core and must close, run
it once more. Then, in any PowerShell:

```powershell
git clone https://github.com/PyDevices/micropython-pydevices.git
cd micropython-pydevices
python build_mp.py --port windows --variant pydevices --modules all
.\builds\windows\pydevices\micropython.exe
```

That's all you type. `build_mp.py` finds MSYS2 in `C:\msys64` (set
`MSYS2_ROOT` if it's elsewhere), puts its tools first on the build's `PATH`,
and uses its `bash`. Left to `PATH`, `bash` from PowerShell is WSL's, which
would run the build inside Linux. Without MSYS2 it stops and prints the
install lines above.

A few things differ from Linux:

- Type `python build_mp.py`, not `./build_mp.py`: Windows doesn't read the
  `#!` line, and python.org's Python is `python` or `py`, never `python3`.
  If `python` opens the Microsoft Store, turn off the `python.exe` and
  `python3.exe` App Execution Aliases (Settings, Apps, Advanced app
  settings, App execution aliases), or type `py` instead.
- Use `--variant pydevices` for the Windows display and audio backends
  (`displaydev.windisplay`, `audiodev.win_audio`). They need `uwin32`, which
  only that variant freezes, so on `dev` or the default they fail to import.
- Only the windows port builds natively. Build esp32, rp2, webassembly and
  unix from WSL, as on Linux.
- The installer, `curl -fsSL https://pydevices.github.io/install.sh | sh`,
  runs in Git Bash (Start menu, Git Bash), not PowerShell. A plain
  `git clone`, as above, does the same job.

The `micropython.exe` it builds needs only Windows' own DLLs, so you can run
it from any shell, MSYS2 or not.

## Boards we build for

Every board here is one of upstream's, with at most a small variant of ours
on top. Add `--modules` to any of these:

| Board | Command |
|---|---|
| Waveshare ESP32-P4-WIFI6-DEV-KIT | `--port esp32 --board ESP32_GENERIC_P4 --variant WIFI6_DEV_KIT --flash 16MB` |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-4B | `--port esp32 --board ESP32_GENERIC_P4 --variant PRE_REV3_C6_WIFI --flash 32MB` |
| Waveshare ESP32-S3-Touch-LCD-7 | `--port esp32 --board ESP32_GENERIC_S3 --variant LCD_7 --flash 8MB` |
| Waveshare ESP32-S3-Touch-LCD-4.3 | `--port esp32 --board ESP32_GENERIC_S3 --variant SPIRAM_OCT --flash 8MB` |
| LilyGO T-Embed S3 | `--port esp32 --board ESP32_GENERIC_S3 --variant T_EMBED --flash 16MB` |
| WT32-SC01 Plus | `--port esp32 --board ESP32_GENERIC_S3 --flash 16MB` |
| Raspberry Pi Pico (RP2040) | `--port rp2 --board RPI_PICO` |
| Raspberry Pi Pico 2 (RP2350) | `--port rp2 --board RPI_PICO2` |

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
name ulab when you name audiocomponents, tflite or vision, or use `all`.
`all` is every module except the opt-in ones (an `OPT_IN` file says why):
tflite and vision are built only when named.

On esp32 and rp2 the build may grow the firmware's share of flash to fit the
image (on esp32 the app partition, on rp2 the region before the filesystem),
leaving 1/32 of the flash as headroom, 64 KB to 256 KB. That moves the
filesystem, so a board flashed with that image comes up with an empty one.
`--no-autosize` refuses instead. On a Pico, `all` is far too big either way;
name what your project uses.

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
   and `T_EMBED` become variants of `ESP32_GENERIC_S3`, and `WIFI6_DEV_KIT`
   one of `ESP32_GENERIC_P4`. That file can also set the board's C
   defines: `WIFI6_DEV_KIT` moves the USB device to the P4's full-speed
   controller with `MICROPY_HW_USB_HS=0`, so other P4 boards keep theirs on
   the high-speed one.

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
| `modules/` | The module manifest, `all/`, the modules that live only here (`castif/`, `h264enc/`, `jpegio/`, and `tflite/` and `vision/`, [esp-vision's](esp-vision.md)), and the rest, linked or cloned. |
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
