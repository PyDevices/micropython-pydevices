# The build plan

**Status:** built, 2026-10-05. Steps 1 to 5 are done and step 6 was dropped (see "Order of work"); this page now records how the build works and why.

You build PyDevices firmware with one command:

```bash
./build_mp.py --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI --flash 16MB --modules audiodsp,displayif,/home/you/earful
```

Leave out `--port` and it walks you through port, board and variant, the way
the old `build_mp.sh` did; leave out `--board` on a port that has boards
(esp32, rp2) and it asks for that. `--variant` is always optional, as with
upstream's `make`, and unix, windows and webassembly take no `--board`. Leave out `--modules` and it lists what's in `modules/` and
asks. Our modules go by short name, anything else by full path, and there are
no recipe files: what goes into a build is what you typed. Anything it doesn't
recognise goes straight to `make`.

Boards come from upstream. You don't write a board definition for a new board;
you pick upstream's generic one, tell it the flash size, and, only where a
board really differs, add a small variant on top. If the image outgrows the
app partition, the build grows the partition and builds again.

## The layout

```text
micropython-pydevices/
├── UPSTREAM                 the MicroPython commit we build on (b70ae1aca657, v1.30.0-preview-160)
├── VERSION                  our version (scheme still open, below)
├── build_mp.py              the one command; Python so it runs anywhere
├── modules.lock             one line per module: name, git URL, commit (and lvgl-bindings and omnipython-lib, below)
├── deps.lock                one line per dependency: name, URL, version (ESP-IDF v5.5.4, emsdk 6.0.1, SDL2 2.30.10)
├── micropython/             ignored: symlink to a sibling checkout, or a clone of UPSTREAM
├── deps/                    ignored: esp-idf/, emsdk/, SDL2/, symlinked or fetched on demand
├── modules/
│   ├── manifest.py          tracked: reads the module list build_mp.py sets, includes each, raises on a missing one
│   ├── all/manifest.py      tracked: every sibling with a root manifest.py, except opt-in ones
│   ├── castif/              tracked: lives only here (moves from usermods/); ESP32-P4 only
│   ├── jpegio/              tracked: lives only here (moved from displayif, 2026-10-06); every port
│   ├── usbhost/             tracked: lives only here (moved from usbif, 2026-10-19, history kept); ESP32-S2, S3 and P4
│   ├── mdns/                tracked: lives only here; esp32, rp2, unix, windows
│   ├── audioif/, cameraif/, displayif/  tracked: moved here 2026-10-19 from their own repos, history kept
│   ├── library.py           tracked: registers omnipython-lib for the manifests that require from it
│   ├── pydevices/, palettes/, pdwidgets/, audiocomponents/   tracked: a manifest each, requiring the package(s) from omnipython-lib
│   ├── micropython-lib/     tracked: a manifest requiring the upstream packages our APIs lean on
│   ├── audiodsp -> ../../audiodsp          ignored symlinks to sibling checkouts,
│   ├── lvgl-micropython,                    clones at the modules.lock commit anywhere else
│   ├── pygraphics, ulab
│   ├── .omnipython-lib      ignored: the library repository, a link or a clone at its modules.lock commit
│   ├── .lvgl-bindings       ignored: what lvgl-micropython compiles; a link, or a sparse fetch at its modules.lock commit
│   └── jpegio                later: see "Open"
├── patches/
│   ├── apply_patches.py     the overlay and the modules' own patches, applied once as one local commit
│   ├── micropython/         0001-…patch to 0038-…patch (no 0026)
│   └── tinyusb/             patches to the TinyUSB the esp32 port builds (S2, S3, P4)
├── variants/                only where a board truly differs from upstream
│   ├── unix/  windows/      pydevices/, vst3-engine/ (as today)
│   ├── webassembly/         pydevices/, and wasmbridge moves in here from usermods/
│   └── esp32/
│       ├── sdkconfig, sdkconfig.<chip>   every esp32 build, then every build for that chip
│       └── <BOARD>/<VARIANT>/            a board's own delta, only where one is left
├── builds/                  ignored: <port>/[<board>/]<variant>/, the generated board dir inside
├── scripts/                 maintainer scripts
├── docs/
└── .devcontainer/
```

When the repositories are already checked out side by side, the real
checkouts stay where they are, and `modules/<name>` and `micropython/` are
symlinks to them.
Anywhere else, [`build_mp.py`](../build_mp.py) clones what's missing: modules at their
`modules.lock` commit, MicroPython at the `UPSTREAM` tag, and the toolchains
at their `deps.lock` version. Beside such checkouts, `deps/` links to the
sibling `esp-idf/`, `emsdk/` and `SDL2/`. A build fetches and checks only the toolchain
its port uses: ESP-IDF for esp32, emsdk for webassembly, SDL2 for windows when
displayif is selected, nothing for unix or rp2. If that one isn't the locked
version, the build refuses. Nothing is a git
submodule, so a module commit never dirties this repo and the overlay commit
never shows as modified.

lvgl-bindings is pinned in `modules.lock` too, though it isn't a module: it
holds the generated binding and the LVGL that lvgl-micropython compiles. A
build with lvgl-micropython puts it at `modules/.lvgl-bindings`, a link to the
sibling checkout or, anywhere else, a fetch of its `modules.lock` commit with
only what gets compiled (tens of MB, not LVGL's history). The build hands it
to lvgl-micropython as `BINDINGS_DIR`, so lvgl-micropython's own
`LVGL_BINDINGS_COMMIT` and fetch never come into a build made here. A sibling
checkout is built as it is, like every other module; when it isn't
lvgl-micropython's pinned commit, the build prints a note. The build record
has a row for it, its commit and whether it was dirty, beside the modules'.

## How a build runs

`build_mp.py` fills in whatever is missing from `micropython/`, `modules/` and
`deps/`, then prepares the MicroPython tree if it isn't prepared already. It
puts your module list in an environment variable that [`modules/manifest.py`](../modules/manifest.py)
reads, and calls the port's own `make` with `FROZEN_MANIFEST=modules/manifest.py`
and, where you asked for one of ours, `VARIANT_DIR`.

On esp32 it also writes a thin board dir into the build dir, the way cmods'
`build_mp.sh` did. That dir includes upstream's board and variant unchanged,
links the rest of the board's files through, and appends our sdkconfig
**last**: the flash size and partition table, then the fragments below.
Last matters, because kconfgen takes the last assignment, and a fragment
listed earlier is silently overridden.

The fragments are found by convention, and each is applied only if it
exists. Every place that can carry them uses the same two names: `sdkconfig`
for every esp32 build, `sdkconfig.<chip>` (the chip read from the board's
`board.json`, such as `esp32p4`) for one chip. They stack in this order:

1. [`variants/esp32/`](../variants/esp32/): PyDevices on any esp32, then on that chip. The S3's
   Wi-Fi, lwIP and PSRAM settings and the P4's NimBLE-over-C6 and
   cache-writeback settings live here.
2. Each selected module's root, in the order you listed them: what that
   module needs. usbhost's USB host settings and cameraif's OV5647 driver live
   in their own module directories.
3. `variants/esp32/<BOARD>/<VARIANT>/`: what's left that really is the board,
   such as the LCD-7's cache tuning. A variant of ours there carries an
   `mpconfigvariant.cmake` that includes the upstream variant it builds on,
   and may carry an `mpconfigboard.h`, included before the board's own, for
   a C define the ESP-IDF components read too (they see the board header
   but not `MICROPY_DEF_BOARD`; `WIFI6_DEV_KIT`'s `MICROPY_HW_USB_HS` is
   one). It may also carry a `manifest.py`, which the build freezes instead
   of `modules/manifest.py`: it includes that file and adds what only this
   board freezes. `WIFI6_DEV_KIT_H2`'s adds pydevices' USB packages and
   midiports, so its apps run without a mip install.

On the S2, S3 and P4 the build also patches TinyUSB, which the port takes
from the IDF component manager and so the overlay can't reach. It checks out
`deps/espressif__tinyusb` at its `deps.lock` tag, which must be the one patch
0014 points the port's manifest at, applies [`patches/tinyusb/`](../patches/tinyusb/) as one local
commit, and adds a one-file component to the board dir whose manifest gives
`espressif/tinyusb` an `override_path` to that copy. The configure log says
`Using component placed at .../deps/espressif__tinyusb`, and the build record
names the TinyUSB it built. (A component of the same name in
`EXTRA_COMPONENT_DIRS` isn't enough: the managed copy is still fetched and
added after it, and wins.)

A `partitions.csv` is found the same way, the most specific winning
(`<BOARD>/<VARIANT>/partitions.csv`, then `partitions.<chip>.csv`, then
`partitions.csv`); without one, the board's own table is used.

Nothing in `build_mp.py` names a version or a setting. A pin moves by editing
a lock file and a setting by editing a fragment, and the script never changes
for either. `make` gets
it as `BOARD_DIR=`. Upstream's tree is never edited, so the ports need no
patch for this.

If the app doesn't fit its partition, the build reads the size out of the
error, grows the app partition to the image plus headroom (1/32 of the flash,
64 KB to 256 KB), rounded up to 64 KB, moves every partition after it, and builds once more against
that table, which lives only in the build dir. It then prints the new layout
and warns that the filesystem moved. This port puts the filesystem after the
last partition, so a board flashed with the new layout comes up with an empty
filesystem. Dev boards hold nothing we keep, so this is on by
default; `--no-autosize` refuses instead and prints the table that would fit.

rp2 gets the same treatment. When the link overflows the `FLASH` region, the
build reads the linker's memory table, gives the firmware the image plus the
same headroom, and builds once more with the rest as the filesystem
(`MICROPY_HW_FLASH_STORAGE_BYTES`, recorded in the build record).

Output lands in `builds/<port>/[<board>/]<variant>/`. The last level is always
there, so one build never nests inside another: with no `--variant` it's the
name upstream's `make` uses, `standard` on unix, windows and webassembly and
`default` for a board. Each dir holds a record of what
went in: each module's path and commit. Build the same target with a
different module set, or with a module at another commit, and it wipes that
dir and rebuilds, and says which module moved and from where to where.
Uncommitted edits to a module build incrementally, as make and ninja handle
them. One dir
per target, never a stale cache (idf.py keeps `-D` values between runs), and
no pile of experiment dirs.

`all` is a module like any other, [`modules/all/`](../modules/all/), so the logic of "everything"
lives in its own manifest, which we can change without touching the build
script. A module with an `OPT_IN` file is the one exception: `all` leaves it
out, and it's built only when named. tflite and vision are opt-in, for their
size ([docs/esp-vision.md](esp-vision.md)).

A module never brings another module. ulab is a module like any other,
pinned only by `modules.lock`; audiodsp, which never used it, no longer names
its own copy, so nothing is compiled twice. Nothing checks dependencies
either: audiocomponents' instruments import ulab, and tflite and vision
compile against it, so name ulab with them, or use `all`.

Our desktop and browser variants (`unix`, `windows` and `webassembly`
`pydevices`) freeze the universal desktop board config themselves, from
`modules/pydevices/board_configs/desktop`, so `--variant pydevices` is a
complete desktop runtime and `all` stays all. Board images never carry a board
config, and neither does mpvst's `vst3-engine`, which has its own.

castif is the one module whose only home is this repo, so [`modules/castif/`](../modules/castif/) is
tracked here rather than linked. It's ESP32-P4 only. Its H.264 encoder moved
into [`modules/h264enc/`](../modules/h264enc/) (the media roadmap's Phase 3), an ESP-IDF component
that has to be added before `project()` runs, so the generated board dir adds
`h264enc/components/h264enc_esp_h264` to `EXTRA_COMPONENT_DIRS` whenever
h264enc is selected: any selected module's `components/<name>/` is added that
way, by convention. Its own glue skips every other port and chip,
the way audioif's skips non-esp32 ports, so `all` needs no exception for it.

## Python-only packages come from omnipython-lib

pydevices, palettes, pdwidgets and audiocomponents' two packages are Python
only, so they are not checkouts in `modules/`: they are packages of
omnipython-lib, the repository of our Python-only packages (the pydevices
repository, until its rename). `modules.lock` pins it like a module
(`omnipython-lib`), and a build that needs it asks its `tools/libbuild.py
build --tree-only` for the library tree: one directory per package with a
`manifest.py` beside its sources. `modules/library.py` registers that tree
with `add_library("omnipython-lib", ...)`, and a manifest then says
`require("palettes", library="omnipython-lib", opt=3)`. In the workspace the
checkout is a link to the sibling `pydevices`, which wins over the pin like
every other module; anywhere else it is a clone at the pinned commit. The tree
goes to `deps/omnipython-lib`, and the outside packages the library builds from
other repositories at a tag (audiocomponents' instruments and effects) are
cloned once into `deps/omnipython-lib-sources`.

You still choose per build: name `pydevices`, `palettes`, `pdwidgets` or
`audiocomponents` in `--modules` and it's frozen (each is a small manifest in
`modules/` that requires its packages); leave it out and the board installs it
with mip. `all` takes them, because all means all. Any other package of the
library can be named the same way: `--modules appkit`, or a board config such
as `esp32-s3-touch-lcd-7` with its drivers, which the package requires. A
package with C stays a module (pygraphics, the device modules).

`modules/pydevices` freezes what `lib/` always held: the library's `pydevices`
and `bledev`, plus the host-only files of `audiodev`, `bledev`, `displaydev`
and `multimer` that mip ships in `pydevices-desktop` (not its `mip.py`,
`pngio.py` or `mdns.py`, which would stand in front of the firmware's own).
Boards still carry those host-only files; leaving them out is a separate,
smaller change.

`modules/micropython-lib` freezes the upstream packages the aligned APIs need
(`aioble`, `sdcard`, `bmi270`, `__future__`, `usb-device` and its device
classes) with `require()` from the micropython-lib checkout the MicroPython
tree carries, so an OmniPython firmware never asks anyone to `mip.install`
them. It is in `all`.

CircuitPython-compatible builds freeze the same way. A module that names no
`c_module()` and has a `manifest.py` (palettes, pdwidgets, pydevices,
`lvgl-circuitpython`'s `fs_driver`) is frozen, and so is any library package
you name; `build_mp.py` writes one `frozen-manifest.py` into the build
directory and hands it to CircuitPython's make as `FROZEN_MANIFEST`. That
manifest starts with the unix port's coverage manifest on the unix port and
with the board's own `FROZEN_MPY_DIRS` content on a board that has some
(setting `FROZEN_MANIFEST` beats the manifest CircuitPython would make from
them, so the build freezes them itself). C modules still reach CircuitPython
through `USER_C_MODULES`, and `micropython-lib` is MicroPython's own and is
left out.

`scripts/freeze_list.py list` evaluates a build's manifest and writes every
file it freezes, by name, with the hash of its source and of what is frozen
(a required package's files get a `__version__` line, as mip installs them),
and the version of every package a `require()` brought in; `compare` says what
differs between two lists: run it on a build's manifest before and after a
change to see exactly what the change did to the firmware.

## Order of work

1. **Reorganise this repo**, on a branch in its own worktree. That covers the
   tree above, `modules.lock`, `deps.lock`, the two module manifests, `apply_patches.py`
   (replacing `apply.sh` and `tools/prepare-micropython.sh`, still applying
   usbif's and cameraif's patches) and the `.gitignore`. The patch profiles
   go: every build applies the whole series, and CI checks that the series
   applies clean to the pin. `manifests/` goes:
   the kitchen sink becomes `modules/all`, and the presets turn into
   `--modules` lists. `usermods/castif` moves to `modules/castif`, and
   audiocomponents gets the root `manifest.py` it lacks.
2. **Write `build_mp.py`**, with the generated esp32 board dir, `--flash`,
   and autosize, ported from cmods' `build_mp.sh`.
3. **Prove the modules on stock upstream builds.** That means unix and
   windows (stock variants), webassembly (our variant), `ESP32_GENERIC_S3`
   `SPIRAM_OCT`, and `ESP32_GENERIC_P4` in both `C6_WIFI` (your DEV-KIT) and
   `PRE_REV3_C6_WIFI` (the panel), with no delta but `--flash` and autosize.
   The gate is that for the same module set, each build's module list matches
   today's, and the S3 and the DEV-KIT boot and import what they carry.
4. **Repoint what calls the old way:** the interpreter build script,
   mpvst's engine build (its modules listed by name plus
   `<mpvst>/vstaudio,<mpvst>/vstui`; not `all`, which now freezes pydevices),
   wokwi's stage script, earful's build, the README and [`newcomers.md`](newcomers.md).
5. **Retire `boards/`.** Done 2026-10-05. The four boards' sdkconfig lines
   sorted into the three homes above: chip settings into [`variants/esp32/`](../variants/esp32/),
   usbif's and cameraif's into their repos (usbif's board-header lines became
   compile definitions in its own CMake), and two board variants of
   `ESP32_GENERIC_S3`, not one: the LCD-7's cache tuning, and the T-Embed's
   flash auto-suspend, which IDF supports only on particular flash chips. The
   partition tables went; the S3 keeps a coredump partition through
   [`variants/esp32/partitions.esp32s3.csv`](../variants/esp32/partitions.esp32s3.csv), found by the same convention as
   the fragments. Flash core dumps are off in standard builds (2026-10-20): `--coredump`
   layers [`sdkconfig.coredump`](../variants/esp32/sdkconfig.coredump) last, and refuses a board
   whose table has no `coredump` partition (today everything but the S3). The S3
   table keeps its 64 KB slot unused in a standard build, so no filesystem moves.
6. **Dropped (2026-10-05): the VARIANT_DIR spike.** Teaching the esp32 and rp2 ports to
   take a variant from outside the board dir would make the generated dir
   tidier, not possible; it already works without it. It isn't planned as an
   upstream PR.

## CircuitPython-compatible builds

The same command builds OmniPython CP (CircuitPython-compatible) firmware:
CircuitPython, with our C modules compiled in.

```bash
./build_mp.py --interpreter circuitpython --port raspberrypi \
    --board adafruit_feather_rp2040 --modules pygraphics
./build_mp.py --interpreter circuitpython --port unix --modules pygraphics
./build_mp.py --interpreter circuitpython --port espressif \
    --board adafruit_qtpy_esp32_pico --modules audiodsp
```

Ports and boards are CircuitPython's own (`raspberrypi`, `atmel-samd`,
`espressif`, `unix` and the rest; boards by their directory names under `ports/<port>/boards/`).
Output lands in `builds/circuitpython/<port>/<board or variant>/`, with the
same build record; unix builds CircuitPython's default variant, `coverage`,
unless you name another. Arguments for make, such as `CIRCUITPY_ULAB=0` to
free flash on a small board, go on the end and are part of the record: a
build with different ones starts from an empty build dir, because
CircuitPython's generated module table would otherwise keep the old set.

It is a flag rather than a second command because everything around the
interpreter is shared: the module names and `modules.lock`, the workspace
links, the output layout and the build record. What differs is one function:

- **The checkout.** CircuitPython is pinned in `deps.lock` like a toolchain
  (`11.0.0-alpha.1`, whose core is MicroPython 1.29) and cloned into
  `deps/circuitpython`, always our own clone, never a link to a sibling
  checkout. A build fetches only the submodules its target needs, with
  CircuitPython's own `tools/ci_fetch_deps.py` (a board's name, or `tests`
  for unix), and its build tools go in `deps/circuitpython-venv` from its
  `requirements-dev.txt`.
- **The modules go in as `USER_C_MODULES`**, not a manifest. From the 1.29
  merge on, CircuitPython's `py/py.mk` carries MicroPython 1.29's
  `USER_C_MODULES` block unchanged, so a module's `micropython.mk` is the one
  our MicroPython builds read. CircuitPython freezes Python its own way
  (`FROZEN_MPY_DIRS`), so a module with no `micropython.mk` is refused for
  now rather than left out quietly. Each module is handed to make through a
  link named for it, in `usermods/` in the build dir: make puts a module's
  objects under a directory named like the module's, and two worktrees
  checked out under the same branch name would otherwise share one.
- **espressif brings its own ESP-IDF.** CircuitPython's espressif port builds
  with the ESP-IDF in `ports/espressif/esp-idf` (6.0 for 11.0.0-alpha.1), not
  the `esp-idf` in `deps.lock`. Before make, the build initialises that tree's
  submodules shallowly (ESP-IDF's CMake would otherwise clone them with their
  whole history, gigabytes for `esp32-wifi-lib` alone), runs its `install.sh`
  for the board's chip (compilers go in `~/.espressif`, shared with any
  ESP-IDF of the same version) and puts ESP-IDF's Python requirements in
  `deps/circuitpython-venv`.
- **Our patches and boards go on first.** [`patches/circuitpython/*.patch`](../patches/circuitpython/)
  are applied to `deps/circuitpython` and each `boards/circuitpython/<port>/<board>`
  is linked into the port's `boards/`, once each; the build record lists the
  patches. The boards are ones CircuitPython has no definition for:
  `feather_rp2040_lvgl` (room for LVGL), `waveshare_esp32s3_touch_lcd_7`
  (its 800x480 panel as `board.DISPLAY`, one 4 MB app slot),
  `lilygo_twatch_s3_single_app` (the T-Watch S3 with one 4 MB app slot, so LVGL
  fits), and the
  ESP32-P4 boards `waveshare_esp32p4_wifi6_dev_kit` (a revision 3 chip,
  console on USB Serial/JTAG) and `waveshare_esp32p4_wifi6_touch_lcd_4b`
  (revision 1, console on the CH343 UART; its panel isn't set up yet), each
  with one 6 MB app slot.
- **A module can ask espressif for ESP-IDF components.** CircuitPython's
  espressif port builds and links a fixed list of ESP-IDF components, which
  leaves out the camera's. With [`0006-espressif-esp-idf-components-a-user-c-module-asks-for.patch`](../patches/circuitpython/0006-espressif-esp-idf-components-a-user-c-module-asks-for.patch),
  a module's `micropython.mk` adds names to `USER_ESP_IDF_COMPONENTS`
  (cameraif: the CSI controller, the ISP and the sensor drivers; jpegio and
  ppa: the P4's JPEG engine and PPA), and the make line's
  `USER_ESP_IDF_COMPONENT_DIRS` adds components ESP-IDF doesn't ship.
  CircuitPython builds with the component manager off, so for cameraif the
  build fetches `esp_cam_sensor`, `esp_sccb_intf` and `cmake_utilities` at
  their `deps.lock` versions into `deps/` and passes them. On CircuitPython,
  jpegio is only the encoder's C API (CircuitPython has its own `jpegio`
  decoder module). The DEV-KIT's board sdkconfig enables the OV5647.
- **mpy-cross is built first, on its own**, for the same reason as on
  MicroPython (the trap below).
- **`BUILD=` is a short link.** make is given `BUILD=build-<board>`, a link in
  the port directory to our build dir: raspberrypi's link step echoes every
  object path in one shell argument, and with a long absolute path in front
  of each it passes Linux's 128 KB limit on one argument.

The banner is CircuitPython's, unchanged: `Adafruit CircuitPython
11.0.0-alpha.1 on <date>; <board name> with <chip>`. A module that has to
behave differently here can test `CIRCUITPY`, which every CircuitPython port
sets, unix included; pygraphics also passes its own define from its
`micropython.mk`, so it knows it came in as a user C module and not through
CircuitPython's shared-bindings.

Two things a module's C has to absorb on CircuitPython's MCU ports: they
append `-Werror` warnings (`sign-compare`, `float-equal`, `cast-align` on
atmel-samd; `float-equal`, `double-promotion`, `cast-align` on espressif)
after the module's flags, so a `-Wno-` meant for them goes on
each object, where it lands last; and atmel-samd has no C heap, so
`malloc()` doesn't link there (use `m_malloc()`).

Builds from Linux or WSL only.

## Traps we already know

**`BUILD=` and the mpy-cross sub-make.** Passed on its own, `BUILD=` reaches
the mpy-cross sub-make and breaks an esp32 link (micropython#19667, still
open). Both build systems skip that sub-make when `MICROPY_MPYCROSS` names a
built mpy-cross (`py/mkenv.mk`, `py/mkrules.cmake`), so `build_mp.py` builds
mpy-cross on its own first and exports its path. No patch needed.

**Upstream's generic esp32 boards give the app 1.94 MB** (`partitions-4MiBplus.csv`),
and the kitchen sink is about 3.4 MB. Autosize handles that, but only if the
flash size is right: gen_esp32part refuses a table larger than the configured
flash. Hence `--flash`.

**A saved `sdkconfig` beats the defaults.** The IDF treats one left in the
build dir as your configuration, so one failed run can pin the wrong
partition table for every run after it. The build deletes it first, and
afterwards checks that the configured table is the one we chose, failing
loudly if it isn't.

**`make clean` on esp32 is `idf.py fullclean`**, which deletes the whole
port's `managed_components/`. Never run it against a build dir that doesn't
exist yet.

**Two esp32 builds at once in one port race** on `managed_components/`, so
`build_mp.py` builds esp32 one at a time.

## Open

Deferred, because the build script doesn't need them:

- **The version scheme** and **what a release carries.**
- **A CI leg that builds the union** on one desktop port, so building only
  what you ask for doesn't lose the cross-port check that caught usbif's
  breaks in September.
- **jpegio** left displayif on 2026-10-06 for [`modules/jpegio`](../modules/jpegio/) here, not a repo of its own. Whether esp-vision's
  sensor and H.264 replace cameraif (and castif's encoder) stays open. esp-vision's
  `tflite` and image stack are in as modules; where `sensor` stands is in
  [esp-vision.md](esp-vision.md).
- **CircuitPython-compatible builds** take C modules only, so far; see
  [CircuitPython-compatible builds](#circuitpython-compatible-builds).
