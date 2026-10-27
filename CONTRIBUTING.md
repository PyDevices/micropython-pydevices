# Contributing to micropython-pydevices

This repository holds our patches to one pinned MicroPython release, our
variants, the C modules that live only here, and the command that builds it all.
A documentation fix, a provenance update, or a narrowly scoped variant or module
fix is a good first contribution.

## Where things are

| Path | What's there |
|---|---|
| `build_mp.py` | The build command. |
| `UPSTREAM` | The MicroPython commit the patches apply to. |
| `modules.lock`, `deps.lock` | What gets fetched when it's missing, lvgl-bindings included. |
| `modules/` | The module manifest, `all/`, `tflite/` and `vision/` (esp-vision's), the modules linked or cloned, and the C modules that live only here: `audioif`, `cameraif`, `castif`, `displayif`, `h264enc`, `jpegio`, `mdns`, `pngio`, `ppa`, `tsmux`, `usbhost`. Each has its own README and tests. |
| `modules/.lvgl-bindings` | The lvgl-bindings lvgl-micropython compiles: the sibling checkout, linked, or `modules.lock`'s commit, fetched. |
| `patches/` | The patch series and `apply_patches.py`. |
| `variants/`, `boards/` | Our variants and CircuitPython boards. |
| `scripts/` | `exercise_build.sh`, `release_firmware.py`, `stage_firmware_page.py`, `serve_workbench.sh`, `freeze_list.py`, `update_provenance.py`. |
| `.devcontainer/` | One dev container per port. |
| `docs/` | The documentation site. |

The reference pages in [the docs](https://github.com/PyDevices/micropython-pydevices/blob/main/docs/index.md)
describe what each of these does.

## Rules

- Upstream is pinned. Moving `UPSTREAM` re-validates every patch.
- Never hand-edit a patched tree. Change the patch and prepare again.
- Patches are justified in their headers.
- `build_mp.py` names no version and no setting. Pins move in the lock files, and
  esp32 settings live in sdkconfig fragments it finds by convention.
- Publishing binaries is a separate decision from this source repository.

## Run the tests

`build_mp.py`'s unit tests decide things before any build, such as when a build
directory has to be wiped. They need no toolchain:

```bash
python3 -m unittest test_build_mp test_freeze_list -v
python3 -m unittest test_provenance -v
python3 -m unittest test_release_firmware test_stage_firmware_page -v
```

Each module has its own workflow under `.github/workflows/`, named for it.

## Change a patch

Check a patch change with `patches/apply_patches.py --check` before you commit
it. After changing a patch or moving `UPSTREAM`, rewrite the record, because CI
fails on a stale file:

```bash
python3 scripts/update_provenance.py
```

## Exercise a build from a bare clone

[`scripts/exercise_build.sh`](scripts/exercise_build.sh) builds every desktop port and two esp32 boards from
a bare clone, checks the failure paths, and writes a report of what should match
on any machine. It needs no board. For a report on builds you already have, name
their folders:

```bash
scripts/exercise_build.sh --report builds/rp2/RPI_PICO2/default ...
```

## Firmware sets

A firmware set is every board's flashable file, built from one commit of this
repository by the **Release firmware** workflow
([`release-firmware.yml`](.github/workflows/release-firmware.yml); the cells are
in [`scripts/release_firmware.py`](scripts/release_firmware.py), and
`release_firmware.py cells` lists them). Each cell is one `build_mp.py` call; the
`assemble` job checks every file (checksums, the ESP image magic, the app image
inside the flash image, UF2 blocks) and writes `firmware.json`, which records
every file's sha256 and size, the flash offset, and the pins the set was built
from.

### Build a set

Actions > Release firmware > Run workflow. A pull request that touches the
recipe, and a run every Monday, build the same cells and keep the files as a run
artifact for 14 days. They publish nothing.

### Put a set on a draft Release

Tick **draft_release** and give a **tag**. After `assemble`, the `draft` job
attests every file (`gh attestation verify <file> --repo
PyDevices/micropython-pydevices`) and creates a *draft* GitHub Release holding
the flash images, the `.elf` and `.map` of each build, and `firmware.json`. A
draft is seen only by people who can push to the repository, and GitHub makes no
tag until someone publishes it.

The tag is `v<MicroPython version>-<YYYYMMDD>`. A set built from a preview
MicroPython or CircuitPython only takes a tag that starts with `staged-`, so a
preview set can't be published by accident.

### The download page

[`scripts/stage_firmware_page.py`](scripts/stage_firmware_page.py) lays a set out
for the site's `firmware/` folder: `firmware.json`, the flash images the page
offers and the browser flasher fetches, an ESP Web Tools manifest per ESP32
build, and the desktop interpreters' `interpreters.json` from the newest
pydevices final that has one.

```bash
python3 scripts/stage_firmware_page.py <downloaded firmware artifact> <site checkout>/firmware \
    --interpreters-from-release PyDevices/pydevices
```

## Build the docs

```bash
python3 -m venv .venv-docs
.venv-docs/bin/pip install -r docs/requirements.txt
.venv-docs/bin/sphinx-build -W --keep-going -b html docs _site
```
