# Newcomer's guide to micropython-pydevices

This repository builds MicroPython firmware with PyDevices modules in it. It
holds our patches to one pinned MicroPython release, our variants, and the
build command. It is not a fork, and it publishes no Python package.

## Build something

Get it, if you haven't: `curl -fsSL https://pydevices.github.io/install.sh | sh`
clones this repository into `micropython-pydevices` in the current directory
and checks your tools. On Windows, see [On Windows](#on-windows).
Run it somewhere that keeps the clone's path short, under about 60
characters, such as your home directory (`~/micropython-pydevices`): an
esp32 build with many modules hands gcc every include path, and a long
clone path pushes that past a Linux limit
([#97](https://github.com/PyDevices/micropython-pydevices/issues/97)).
Then, from the clone:

```bash
./build_mp.py --port unix --variant pydevices --modules pydevices,displayif,pygraphics
./build_mp.py --port esp32 --board ESP32_GENERIC_S3 --variant SPIRAM_OCT --flash 8MB --modules all
./build_mp.py          # asks for port, board, variant, flash size and modules
```

The first run fetches what it needs. Where the repositories are already
checked out side by side, it links those sibling checkouts instead. An
esp32 build's first run also clones ESP-IDF into the clone's `deps/` (about
770 MB) and installs its toolchains, for every ESP chip, under
`~/.espressif`: about 4 GB, outside the clone (set `IDF_TOOLS_PATH` to put
them elsewhere), and most of the first esp32 build's time. The toolchains
stay put, but ESP-IDF goes with the clone: move it or clone again and the
next esp32 build fetches ESP-IDF again.

The desktop `pydevices` variants freeze a board config that imports
`displaydev`, which is frozen only when `--modules` names `pydevices` (the
repository whose `lib/` holds it) or `all`; without it `import board_config`
fails.

## What each port needs

[`build_mp.py`](../build_mp.py) fetches the big pieces itself: MicroPython, the modules,
ESP-IDF, emsdk, the pico-sdk and picotool, SDL2 for MinGW. What it can't
fetch are your distribution's packages. On Debian or Ubuntu (WSL included):

| Port | Packages |
|---|---|
| every port | `git make cmake python3 gcc g++` |
| unix | `pkg-config libsdl2-dev libffi-dev` (the desktop display) |
| esp32 | `ninja-build python3-venv` |
| rp2 | `gcc-arm-none-eabi libnewlib-arm-none-eabi` |
| windows, cross-built from Linux | `gcc-mingw-w64-x86-64 libtool libltdl-dev autoconf automake` |
| webassembly | `nodejs` |

All of it in one line:

```bash
sudo apt-get update && sudo apt-get install -y git make cmake ninja-build \
    python3 python3-venv python3-pip gcc g++ pkg-config libffi-dev libsdl2-dev \
    gcc-mingw-w64-x86-64 libtool libltdl-dev autoconf automake \
    gcc-arm-none-eabi libnewlib-arm-none-eabi nodejs
```

The installer prints this line cut down to what your machine is missing,
and says which ports build now. Other distributions carry the same tools
under their own names; a build for a port that's still missing something
stops and says so.

### Warnings you can ignore

A build prints a few things that look wrong and aren't:

- Every esp32 and rp2 build with `all` fails its first link on size, then
  autosize builds again. [The model](#the-model) says what that looks like.
- esp32:
  - `warning: unknown kconfig symbol 'XTAL_FREQ_AUTO'` and a run of
    `CONFIG_… was replaced with CONFIG_…` lines: ESP-IDF renamed those
    settings and still maps the old names.
  - `Component lockfile contents have changed`, a CMake warning that asks
    you to mention it in any issue. The build uses its own TinyUSB and a
    newer ESP-IDF than upstream's lockfile records; `build_mp.py` puts the
    lockfile back afterwards.
  - cmake's `Performing Test CMAKE_HAVE_LIBC_PTHREAD - Failed` and `Could
    NOT find Threads`.
  - On the P4, `Detecting C compiler ABI info - failed` and the same for
    CXX, each checked again a moment later and reported `done`.
  - On the first build, `WARNING: Download failure: HTTP Error 502` from
    ESP-IDF's tool installer, which tries again by itself.
- rp2:
  - `No installed picotool with version 2.3.0 found - building from
    source`, once per pass.
  - On the first build, `PICO_TINYUSB_PATH specified, but does not contain
    hw/bsp/rp2040; USB support will be unavailable`. It comes from the pass
    that fetches submodules, before TinyUSB is checked out; the real build
    says `TinyUSB available` and builds it.
  - `PICO_BTSTACK_PATH specified but content not present` and the same for
    `PICO_LWIP_PATH`, on boards without networking or Bluetooth, which
    don't fetch those libraries.
- windows:
  - libffi's configure says `libtool.m4: error: problem compiling CXX test
    program`; it is probing for a C++ compiler it doesn't need, and the
    build goes on.
  - Cross-built from Linux, `configure: WARNING: using cross tools not
    prefixed with host triplet`: there's no MinGW-named `file` command, so
    it uses the plain one.
  - Autoconf's ``The macro `AC_TRY_COMPILE' is obsolete`` and ``The
    preprocessor macro `STDC_HEADERS' is obsolete``, and four `… is
    deprecated [-Wdeprecated-declarations]` warnings, all from libffi's own
    sources.

## On Windows

This section is for building on Windows itself. A Linux or WSL machine
cross-builds the same `micropython.exe` with the same command,
`./build_mp.py --port windows --variant pydevices --modules all`, once the
MinGW packages in [What each port needs](#what-each-port-needs) are
installed; the SDL2 it links against is fetched.

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
| Waveshare ESP32-P4-WIFI6-DEV-KIT, USB device on a USB-C wired to header P6 | `--port esp32 --board ESP32_GENERIC_P4 --variant WIFI6_DEV_KIT --flash 16MB` |
| Waveshare ESP32-P4-WIFI6-DEV-KIT, USB device on its USB-C marked "USB" | `--port esp32 --board ESP32_GENERIC_P4 --variant WIFI6_DEV_KIT_H2 --flash 16MB` |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-4B | `--port esp32 --board ESP32_GENERIC_P4 --variant PRE_REV3_C6_WIFI --flash 32MB` |
| Another ESP32-P4 board, chip revision 3 or later, with an ESP32-C6 for Wi-Fi | `--port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI --flash 16MB` |
| Waveshare ESP32-S3-Touch-LCD-7 | `--port esp32 --board ESP32_GENERIC_S3 --variant LCD_7 --flash 8MB` |
| Waveshare ESP32-S3-Touch-LCD-4.3 | `--port esp32 --board ESP32_GENERIC_S3 --variant SPIRAM_OCT --flash 8MB` |
| LilyGO T-Embed S3 | `--port esp32 --board ESP32_GENERIC_S3 --variant T_EMBED --flash 16MB` |
| WT32-SC01 Plus | `--port esp32 --board ESP32_GENERIC_S3 --flash 16MB` |
| Raspberry Pi Pico (RP2040) | `--port rp2 --board RPI_PICO` (not `--modules all`: see [The model](#the-model)) |
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

With no `--variant`, that last directory is `default` on esp32 and rp2, and
`standard`, upstream's default variant, on unix, windows and webassembly.

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
`--no-autosize` refuses instead.

It finds out by trying: the first link fails, loudly, and only then does it
build again. On esp32 that is a red `Error: app partition is too small for
binary micropython.bin size 0x…`, then `ninja: build stopped`, `make: ***
Error 1` and a Build-Troubleshooting link; on rp2 it is ``region `FLASH'
overflowed by … bytes``, `collect2: error: ld returned 1 exit status` and
nested `Error 2` lines. Then an `autosize:` line says it didn't fit and is
building once more. With `all`, expect that second pass on every board
above; it roughly doubles the build time, since the whole esp32 compile
runs twice.

How much fits depends on the flash. On a Pico 2 (4 MB), `all` builds and
fits, at 93 % of flash with a 1600 KB filesystem. On a Pico (2 MB) it can't:
an image of that size (2.3 MB on the Pico 2) is bigger than the whole flash,
so the build stops at the first link with the overflow error. Name what your
project uses instead: `displayif,jpegio,pygraphics,palettes,pdwidgets`, a
display project's set, builds to about 540 KB and fits with the Pico's
default 1408 KB filesystem untouched.

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
   one of `ESP32_GENERIC_P4`. A variant dir may also hold an
   `mpconfigboard.h`, included before the board's own, for C defines the
   port's ESP-IDF components must see too: `WIFI6_DEV_KIT`'s sets
   `MICROPY_HW_USB_HS (0)`, which moves the USB device to the P4's
   full-speed controller, so other P4 boards keep theirs on the high-speed
   one. A variant dir may carry C sources too, through
   `MICROPY_SOURCE_BOARD` in its `mpconfigvariant.cmake`, the way upstream's
   `ESP32_GENERIC_C2` does: `WIFI6_DEV_KIT_H2` builds on `WIFI6_DEV_KIT` and
   adds a board startup hook that swaps the P4's two full-speed PHYs, so the
   USB device comes out on the kit's USB-C marked "USB".

The partition table is found the same way, the most specific
`partitions.csv` winning ([`variants/esp32/partitions.esp32s3.csv`](../variants/esp32/partitions.esp32s3.csv) adds a
coredump partition on the S3); without one, the board's own is used. Either
way autosize grows the app partition when the image doesn't fit.

## Where things are

| Path | What it is |
|---|---|
| [`build_mp.py`](../build_mp.py) | The build command. |
| [`UPSTREAM`](../UPSTREAM) | The MicroPython release the patches apply to. |
| [`modules.lock`](../modules.lock), [`deps.lock`](../deps.lock) | What gets fetched when it's missing. |
| [`modules/`](../modules/) | The module manifest, `all/`, the modules that live only here (`castif/`, `h264enc/`, `jpegio/`, and `tflite/` and `vision/`, [esp-vision's](esp-vision.md)), and the rest, linked or cloned. |
| [`patches/`](../patches/) | The patch series and [`apply_patches.py`](../patches/apply_patches.py). |
| [`variants/`](../variants/) | Our unix, windows and webassembly variants, and the esp32 settings and board variants. |

## Boundaries

The upstream tag is a compatibility boundary: moving `UPSTREAM` means
re-validating every patch. Never hand-edit a patched checkout; change the
patch and prepare again ([`patches/apply_patches.py`](../patches/apply_patches.py)).

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
