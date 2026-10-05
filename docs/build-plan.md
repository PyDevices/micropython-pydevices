# The build plan

**Status:** draft for Brad's review, 2026-10-05. Nothing here is built yet.

You build PyDevices firmware with one command:

```bash
./build_mp.py --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI --flash 16MB --modules audiodsp,displayif,/home/you/earful
```

Leave out `--port`, `--board` or `--variant` and it asks you, the way the old
`build_mp.sh` did. Leave out `--modules` and it lists what's in `modules/` and
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
├── micropython/             ignored: symlink to the workspace's checkout, or a clone of UPSTREAM
├── deps/                    ignored: esp-idf/, emsdk/, SDL2/, symlinked or fetched on demand
├── modules/
│   ├── manifest.py          tracked: reads the module list build_mp.py sets, includes each, raises on a missing one
│   ├── all/manifest.py      tracked: every sibling with a root manifest.py, minus the ones it skips
│   ├── castif/              tracked: lives only here (moves from usermods/); ESP32-P4 only
│   ├── audiodsp -> ../../audiodsp          ignored symlinks in the workspace,
│   ├── audioif  -> ../../audioif           clones at the modules.lock commit anywhere else
│   ├── cameraif, displayif, lvgl-micropython, palettes, pdwidgets,
│   ├── pydevices, pygraphics, ulab, usbif
│   └── jpegio, audiocomponents   later: see "Open"
├── patches/
│   ├── apply_patches.py     the overlay and the modules' own patches, applied once as one local commit
│   └── micropython/         0001-…patch to 0016-…patch, as today
├── variants/                only where a board truly differs from upstream
│   ├── unix/  windows/      pydevices/, vst3-engine/ (as today)
│   ├── webassembly/         pydevices/, and wasmbridge moves in here from usermods/
│   └── esp32/<BOARD>/<VARIANT>/   a board's delta: an sdkconfig fragment, maybe a partition table
├── builds/              ignored: <port>/<board>/<variant>/, the generated board dir inside
├── scripts/                 maintainer scripts
├── docs/
└── .devcontainer/
```

In the workspace, the real checkouts stay where they are, as siblings under
`~/gh/pydevices/`, and `modules/<name>` and `micropython/` are symlinks to them.
Anywhere else, `build_mp.py` clones what's missing: modules at their
`modules.lock` commit, MicroPython at the `UPSTREAM` tag. Nothing is a git
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
**last**: the flash size, the partition table, and any delta from
`variants/esp32/`. Last matters, because kconfgen takes the last assignment,
and a fragment listed earlier is silently overridden (cmods#29). `make` gets
it as `BOARD_DIR=`. Upstream's tree is never edited, so the ports need no
patch for this.

If the app doesn't fit its partition, the build reads the size out of the
error, grows the app partition to the image rounded up to 64 KB plus 256 KB
of headroom, moves every partition after it, and builds once more against
that table, which lives only in the build dir. It then prints the new layout
and warns that the filesystem moved. This port puts the filesystem after the
last partition, so a board flashed with the new layout comes up with an empty
filesystem (cmods#30). Dev boards hold nothing we keep, so this is on by
default; `--no-autosize` refuses instead and prints the table that would fit.

Output lands in `builds/<port>/<board>/<variant>/`, with a record of what
went in: each module's path and commit. Build the same target with a
different module set and it wipes that dir and rebuilds, and says so. One dir
per target, never a stale cache (idf.py keeps `-D` values between runs), and
no pile of experiment dirs.

`all` is a module like any other, `modules/all/`, so the logic of "everything"
lives in its own manifest, which we can change without touching the build
script.

castif is the one module whose only home is this repo, so `modules/castif/` is
tracked here rather than linked. It's ESP32-P4 only: its H.264 encoder is an
ESP-IDF component that has to be added before `project()` runs, so the
generated board dir adds `castif/castif_h264` to `EXTRA_COMPONENT_DIRS`
whenever castif is selected, and `all` takes castif only on a P4.

## pydevices, frozen or not

You choose per build. `--modules pydevices` freezes it; leave it out and the
board installs it with mip as today. `all` skips it, because `pydevices/lib`
moves too fast to freeze by default. The wasm variant's manifest does freeze
it, since that's where it pays: pages load without fetching it.

Later, once our own package library exists, we'd rather say
`require("pydevices", library=<ours>)`. Then the `modules/pydevices` symlink
goes away, `all` can't grab it by accident, and the wasm manifest still gets it.

## Order of work

1. **Reorganise this repo**, on a branch in its own worktree. That covers the
   tree above, `modules.lock`, the two module manifests, `apply_patches.py`
   (replacing `apply.sh` and `tools/prepare-micropython.sh`, still applying
   usbif's and cameraif's patches) and the `.gitignore`. `manifests/` goes:
   the kitchen sink becomes `modules/all`, and the presets turn into
   `--modules` lists. `usermods/castif` moves to `modules/castif`.
2. **Write `build_mp.py`**, with the generated esp32 board dir, `--flash`,
   and autosize, ported from cmods' `build_mp.sh`.
3. **Prove the modules on stock upstream builds.** That means unix and
   windows (stock variants), webassembly (our variant), `ESP32_GENERIC_S3`
   `SPIRAM_OCT`, and `ESP32_GENERIC_P4` in both `C6_WIFI` (your DEV-KIT) and
   `PRE_REV3_C6_WIFI` (the panel), with no delta but `--flash` and autosize.
   The gate is that for the same module set, each build's module list matches
   today's, and the S3 and the DEV-KIT boot and import what they carry.
4. **Repoint what calls the old way:** the anchor's `build_interpreters.sh`,
   mpvst's engine build (`--modules all,<mpvst>/vstaudio,<mpvst>/vstui`),
   wokwi's stage script, earful's build, the README and `newcomers.md`.
5. **Retire `boards/`.** Each board's delta (the T-Embed, the panel, the
   S3 Touch 4.3, the LCD-7) moves into `variants/esp32/<BOARD>/<VARIANT>/`,
   and the generated board dir carries it.
6. **Optional: the VARIANT_DIR spike.** Teaching the esp32 and rp2 ports to
   take a variant from outside the board dir would make the generated dir
   tidier, not possible; it already works without it. Whether it becomes an
   upstream PR is your call.

## Traps we already know

**Never pass `BUILD=` to an esp32 make.** The mpy-cross sub-make inherits it
and the link breaks (micropython#19667, still open). So `builds/` needs
either a one-line overlay patch that clears `BUILD=` for mpy-cross, the way
upstream already clears `USER_C_MODULES=`, or for esp32 we call `idf.py -B`
directly. The patch is smaller.

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

These are raised, not decided:

- **The version scheme.** `1.29.261004` drops upstream's patch number;
  `1.29.1.261004` keeps it.
- **What a release carries**, and how "a release every time a module releases"
  is triggered.
- **Where `deps/` takes its pins**, ESP-IDF's version in particular.
- **Whether the patch profiles survive.** Preparing applies every patch
  anyway; the profiles now only feed CI's applicability check.
- **A CI leg that builds the union** on one desktop port, so building only
  what you ask for doesn't lose the cross-port check that caught usbif's
  breaks in September.
- **Per-chip settings.** Most of the P4 sdkconfig fragment is
  PyDevices-on-a-P4, not board-specific, and could apply to every P4 build.
- **jpegio** out of displayif into its own repo, and whether esp-vision's
  sensor and H.264 replace cameraif (and castif's encoder).
- **CircuitPython.** It doesn't read our manifests, and none of this reaches
  it yet.
