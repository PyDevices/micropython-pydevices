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
├── UPSTREAM                 the MicroPython tag we build on (v1.29.0)
├── VERSION                  our version (scheme still open, below)
├── build_mp.py              the one command; Python so it runs anywhere
├── modules.lock             one line per module: name, git URL, commit
├── deps.lock                one line per dependency: name, URL, version (ESP-IDF v5.5.4, emsdk 6.0.1, SDL2 2.30.10)
├── micropython/             ignored: symlink to the workspace's checkout, or a clone of UPSTREAM
├── deps/                    ignored: esp-idf/, emsdk/, SDL2/, symlinked or fetched on demand
├── modules/
│   ├── manifest.py          tracked: reads the module list build_mp.py sets, includes each, raises on a missing one
│   ├── all/manifest.py      tracked: every sibling with a root manifest.py, except opt-in ones
│   ├── castif/              tracked: lives only here (moves from usermods/); ESP32-P4 only
│   ├── jpegio/              tracked: lives only here (moved from displayif, 2026-10-06); every port
│   ├── audiodsp -> ../../audiodsp          ignored symlinks in the workspace,
│   ├── audioif  -> ../../audioif           clones at the modules.lock commit anywhere else
│   ├── audiocomponents, cameraif, displayif, lvgl-micropython, palettes,
│   ├── pdwidgets, pydevices, pygraphics, ulab, usbif
│   └── jpegio                later: see "Open"
├── patches/
│   ├── apply_patches.py     the overlay and the modules' own patches, applied once as one local commit
│   └── micropython/         0001-…patch to 0016-…patch, as today
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

In the workspace, the real checkouts stay where they are, as siblings under
`~/gh/pydevices/`, and `modules/<name>` and `micropython/` are symlinks to them.
Anywhere else, `build_mp.py` clones what's missing: modules at their
`modules.lock` commit, MicroPython at the `UPSTREAM` tag, and the toolchains
at their `deps.lock` version. In the workspace `deps/` links to the anchor's
`esp-idf/`, `emsdk/` and `SDL2/`. A build fetches and checks only the toolchain
its port uses: ESP-IDF for esp32, emsdk for webassembly, SDL2 for windows when
displayif is selected, nothing for unix or rp2. If that one isn't the locked
version, the build refuses. Nothing is a git
submodule, so a module commit never dirties this repo and the overlay commit
never shows as modified.

## How a build runs

`build_mp.py` fills in whatever is missing from `micropython/`, `modules/` and
`deps/`, then prepares the MicroPython tree if it isn't prepared already. It
puts your module list in an environment variable that `modules/manifest.py`
reads, and calls the port's own `make` with `FROZEN_MANIFEST=modules/manifest.py`
and, where you asked for one of ours, `VARIANT_DIR`.

On esp32 it also writes a thin board dir into the build dir, the way cmods'
`build_mp.sh` did. That dir includes upstream's board and variant unchanged,
links the rest of the board's files through, and appends our sdkconfig
**last**: the flash size and partition table, then the fragments below.
Last matters, because kconfgen takes the last assignment, and a fragment
listed earlier is silently overridden (cmods#29).

The fragments are found by convention, and each is applied only if it
exists. Every place that can carry them uses the same two names: `sdkconfig`
for every esp32 build, `sdkconfig.<chip>` (the chip read from the board's
`board.json`, such as `esp32p4`) for one chip. They stack in this order:

1. `variants/esp32/`: PyDevices on any esp32, then on that chip. The S3's
   Wi-Fi, lwIP and PSRAM settings and the P4's NimBLE-over-C6 and
   cache-writeback settings live here.
2. Each selected module's root, in the order you listed them: what that
   module needs. usbif's USB host settings and cameraif's OV5647 driver live
   in their own repos.
3. `variants/esp32/<BOARD>/<VARIANT>/`: what's left that really is the board,
   such as the LCD-7's cache tuning. A variant of ours there carries an
   `mpconfigvariant.cmake` that includes the upstream variant it builds on.

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
filesystem (cmods#30). Dev boards hold nothing we keep, so this is on by
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
different module set and it wipes that dir and rebuilds, and says so. One dir
per target, never a stale cache (idf.py keeps `-D` values between runs), and
no pile of experiment dirs.

`all` is a module like any other, `modules/all/`, so the logic of "everything"
lives in its own manifest, which we can change without touching the build
script. A module with an `OPT_IN` file is the one exception: `all` leaves it
out, and it's built only when named. tflite and vision are opt-in, for their
size (docs/esp-vision.md).

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

castif is the one module whose only home is this repo, so `modules/castif/` is
tracked here rather than linked. It's ESP32-P4 only: its H.264 encoder is an
ESP-IDF component that has to be added before `project()` runs, so the
generated board dir adds `castif/castif_h264` to `EXTRA_COMPONENT_DIRS`
whenever castif is selected: any selected module's `components/<name>/` is
added that way, by convention. Its own glue skips every other port and chip,
the way audioif's skips non-esp32 ports, so `all` needs no exception for it.

## Python-only modules, frozen or not

pydevices, palettes, pdwidgets and audiocomponents are Python only. You
choose per build: name one in `--modules` and it's frozen; leave it out and
the board installs it with mip as today. `all` takes them, because all means
all. Today that's what the wasm build wants, since pages load without fetching
anything. For MCU builds `pydevices/lib` moves too fast to freeze as a habit,
so name what you want rather than reaching for `all`.

Later, once the PyDevices/mip repo is retooled into our own package library,
these come in with `require("<name>", library=<ours>)` instead. Their
symlinks then leave `modules/`, so `all` no longer takes them, and a manifest
that wants one (the wasm one) requires it by name.

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
4. **Repoint what calls the old way:** the anchor's `build_interpreters.sh`,
   mpvst's engine build (its modules listed by name plus
   `<mpvst>/vstaudio,<mpvst>/vstui`; not `all`, which now freezes pydevices),
   wokwi's stage script, earful's build, the README and `newcomers.md`.
5. **Retire `boards/`.** Done 2026-10-05. The four boards' sdkconfig lines
   sorted into the three homes above: chip settings into `variants/esp32/`,
   usbif's and cameraif's into their repos (usbif's board-header lines became
   compile definitions in its own CMake), and two board variants of
   `ESP32_GENERIC_S3`, not one: the LCD-7's cache tuning, and the T-Embed's
   flash auto-suspend, which IDF supports only on particular flash chips. The
   partition tables went; the S3 keeps a coredump partition through
   `variants/esp32/partitions.esp32s3.csv`, found by the same convention as
   the fragments.
6. **Dropped (Brad, 2026-10-05): the VARIANT_DIR spike.** Teaching the esp32 and rp2 ports to
   take a variant from outside the board dir would make the generated dir
   tidier, not possible; it already works without it. Whether it becomes an
   upstream PR is your call.

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
loudly if it isn't (cmods#29).

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
- **jpegio** left displayif on 2026-10-06 for `modules/jpegio` here (the
  anchor's media modules roadmap), not a repo of its own. Whether esp-vision's
  sensor and H.264 replace cameraif (and castif's encoder) stays open. esp-vision's
  `tflite` and image stack are in as modules; where `sensor` stands is in
  [esp-vision.md](esp-vision.md).
- **CircuitPython.** It doesn't read our manifests, and none of this reaches
  it yet.
