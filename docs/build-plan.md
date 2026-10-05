# The build plan

**Status:** draft for Brad's review, 2026-10-05. Nothing here is built yet.

You build PyDevices firmware with one command:

```bash
./build_mp.py --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI --modules audiodsp,displayif,/home/you/earful
```

Leave out `--port`, `--board` or `--variant` and it asks you, the way the old
`build_mp.sh` did. Leave out `--modules` and it lists what's in `modules/` and
asks. Our modules go by short name, anything else by full path, and there are
no recipe files: what goes into a build is what you typed. Anything it doesn't
recognise goes straight to `make`.

Boards come from upstream. You don't write a board definition for a new board;
you pick upstream's generic one and, where a board really differs, a small
variant on top of it.

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
│   ├── audiodsp -> ../../audiodsp          ignored symlinks in the workspace,
│   ├── audioif  -> ../../audioif           clones at the modules.lock commit anywhere else
│   ├── cameraif, displayif, lvgl-micropython, palettes, pdwidgets,
│   ├── pydevices, pygraphics, ulab, usbif
│   └── castif, jpegio, mp3, audiocomponents   later: see "Open"
├── patches/
│   ├── apply_patches.py     the overlay and the modules' own patches, applied once as one local commit
│   └── micropython/         0001-…patch to 0016-…patch, as today
├── variants/                only where a board truly differs from upstream
│   ├── unix/  windows/      pydevices/, vst3-engine/ (as today)
│   ├── webassembly/         pydevices/, and wasmbridge moves in here from usermods/
│   └── esp32/<BOARD>/<VARIANT>/   after the VARIANT_DIR spike, below
├── build_dirs/              ignored: <port>/<board>/<variant>/
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

Output lands in `build_dirs/<port>/<board>/<variant>/`, with a record of what
went in: each module's path and commit. Build the same target with a
different module set and it wipes that dir and rebuilds, and says so. One dir
per target, never a stale cache (idf.py keeps `-D` values between runs), and
no pile of experiment dirs.

`all` is a module like any other, `modules/all/`, so the logic of "everything"
lives in its own manifest, which we can change without touching the build
script.

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
   `--modules` lists.
2. **Write `build_mp.py`.**
3. **Prove the modules on stock upstream builds.** That means unix and
   windows (stock variants), webassembly (our variant), `ESP32_GENERIC_S3`
   `SPIRAM_OCT`, and `ESP32_GENERIC_P4` in both `C6_WIFI` (your DEV-KIT) and
   `PRE_REV3_C6_WIFI` (the panel). The gate is that for the same module set,
   each build's module list matches today's, and the S3 and the DEV-KIT boot
   and import what they carry.
4. **Repoint what calls the old way:** the anchor's `build_interpreters.sh`,
   mpvst's engine build (`--modules all,<mpvst>/vstaudio,<mpvst>/vstui`),
   wokwi's stage script, earful's build, the README and `newcomers.md`.
5. **The VARIANT_DIR spike.** Teach the esp32 and rp2 ports to take a variant
   from outside the board dir, in our overlay first. When it works, `boards/`
   retires into `variants/esp32/<BOARD>/<VARIANT>/`, and an upstream PR is
   your call.

## Traps we already know

**Never pass `BUILD=` to an esp32 make.** The mpy-cross sub-make inherits it
and the link breaks (micropython#19667, still open). So `build_dirs/` needs
either a one-line overlay patch that clears `BUILD=` for mpy-cross, the way
upstream already clears `USER_C_MODULES=`, or for esp32 we call `idf.py -B`
directly. The patch is smaller.

**Upstream's generic esp32 boards give the app 1.94 MB** (`partitions-4MiBplus.csv`),
and the kitchen sink is about 3.4 MB. Stock-board proofs in step 3 use module
sets that fit, and the full set waits for a variant with our partition table
(step 5).

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
- **Per-chip P4 settings and `--flash`.** Most of the P4 sdkconfig fragment
  is PyDevices-on-a-P4, not board-specific, and flash size could be an option
  rather than a variant.
- **The new repos** (castif and jpegio out of their current homes, mp3 out of
  the workspace), and whether esp-vision's sensor and H.264 replace cameraif.
- **CircuitPython.** It doesn't read our manifests, and none of this reaches
  it yet.
