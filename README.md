# micropython-pydevices

Build MicroPython firmware with PyDevices modules in it, for any port, with
one command:

```bash
./build_mp.py --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI \
    --flash 16MB --modules displayif,pygraphics,/home/you/earful
```

Boards are upstream's own generic ones; the
[newcomer's guide](docs/newcomers.md#boards-we-build-for) has the command
for each board we use. You name the modules you want: ours
by short name, anything else by its path, or `all`. Leave out `--port` and it
asks you for port, board and variant. Output lands in
`builds/<port>/[<board>/]<variant>/` (with no `--variant`, that last
directory is `default` on esp32 and rp2, `standard` on the desktop ports),
beside a record of every module's commit. [Flash it](docs/newcomers.md#flash-it)
says where `firmware.bin` goes on the board.

Don't have it yet? This clones it into `micropython-pydevices` in the current
directory and checks your tools (Linux, WSL, or Git Bash on Windows):

```bash
curl -fsSL https://pydevices.github.io/install.sh | sh
```

It names the Debian packages each port still needs, with the `apt-get` line
that installs them; [What each port needs](docs/newcomers.md#what-each-port-needs)
is the same list.

On Windows you can build the windows port natively from PowerShell, with
`python build_mp.py --port windows --variant pydevices --modules all`, once
MSYS2 is installed: [On Windows](docs/newcomers.md#on-windows) has the
steps. Everything else builds from WSL. A Linux or WSL machine also
cross-builds that same `micropython.exe`, with the same command and the
MinGW packages from the list above; the SDL2 it links against is fetched.

The same command builds OmniPython (CircuitPython-compatible) firmware, which
is CircuitPython with our C modules compiled in, for CircuitPython's own
ports and boards:

```bash
./build_mp.py --interpreter circuitpython --port raspberrypi \
    --board adafruit_feather_rp2040 --modules pygraphics
```

It takes C modules only, so far; the
[build plan](docs/build-plan.md#circuitpython-compatible-builds) has how it
works.

New here? The [newcomer's guide](docs/newcomers.md) explains the model; the
[build plan](docs/build-plan.md) has the layout, the rules and the traps.

## What's in here

- [`build_mp.py`](build_mp.py) — the one command. It fills in `micropython/`, `modules/`
  and `deps/` if they're missing, prepares the MicroPython tree, and runs
  the port's own `make`.
- [`UPSTREAM`](UPSTREAM) — the MicroPython release everything applies to (v1.29.0).
- [`modules.lock`](modules.lock), [`deps.lock`](deps.lock) — the commits and versions `build_mp.py`
  fetches when a module or a toolchain isn't there. `deps.lock` also pins
  CircuitPython, for `--interpreter circuitpython`. Beside sibling
  checkouts, both are links to those checkouts instead.
- [`modules/`](modules/) — [`manifest.py`](modules/manifest.py) (the manifest every build freezes), `all/`
  (every module), `tflite/` and `vision/` (esp-vision's modules, from the pin
  in `.esp-vision/`; see [docs/esp-vision.md](docs/esp-vision.md)), the modules
  themselves, linked or cloned, and the C modules that live only here:
  - [`castif/`](modules/castif/): casts an ESP32-P4's screen and sound to a TV over Wi-Fi
    Display, or serves them as live HLS.
  - [`h264enc/`](modules/h264enc/): RGB565 frames to H.264 on the ESP32-P4's hardware encoder.
  - [`jpegio/`](modules/jpegio/): JPEG decode and encode, with CircuitPython's `jpegio` API.
  - [`pngio/`](modules/pngio/): PNG encode and decode, RGB565 in and out.
  - [`ppa/`](modules/ppa/): scale, rotate, convert and fill pixel buffers, on the
    ESP32-P4's Pixel Processing Accelerator or in portable C elsewhere.
  - [`tsmux/`](modules/tsmux/): MPEG-TS and HLS segments from H.264.
- [`patches/micropython/`](patches/micropython/) — our patch series (`0001-…` to `0029-…`; there
  is no `0026`, whose fix moved into `patches/tinyusb/`), each justified
  in its own header; [`patches/apply_patches.py`](patches/apply_patches.py) applies it, plus
  each module's own MicroPython patches, once, as one local commit.
- [`patches/tinyusb/`](patches/tinyusb/) — patches to the TinyUSB the esp32 port builds on the
  S2, S3 and P4, which `build_mp.py` applies to `deps/espressif__tinyusb`
  (at its `deps.lock` tag) and builds in place of the port's own copy.
- [`variants/`](variants/) — our variants: `unix/` and `windows/` (`pydevices`,
  `vst3-engine`), and `webassembly/` (`pydevices`, with the `wasmbridge`
  module and a Fetch-backed `requests`), and `esp32/`: the sdkconfig
  fragments and partition table every esp32 or every chip gets, and the few
  board variants of ours (`ESP32_GENERIC_S3` `LCD_7` and `T_EMBED`,
  `ESP32_GENERIC_P4` `WIFI6_DEV_KIT` and `WIFI6_DEV_KIT_H2`).
- [`scripts/exercise_build.sh`](scripts/exercise_build.sh) — builds every desktop port and two esp32
  boards from a bare clone, checks the failure paths, and writes a report of
  what should match on any machine. Needs no board.
- [`provenance.json`](provenance.json) — the patches' checksums and migration record.

What's planned next is in [ROADMAP.md](ROADMAP.md).

## Rules

- Upstream is **pinned**. Moving `UPSTREAM` re-validates every patch.
- Patches are justified in their headers. Never hand-edit a patched tree:
  change the patch and prepare again.
- `build_mp.py` names no version and no setting. Pins move in the lock files,
  esp32 settings in sdkconfig fragments it finds by convention.
- Publishing binaries is a separate decision from this source repository.

Migrated from `PyDevices/cmods` (2026-08-29), which was archived on
2026-09-23.
