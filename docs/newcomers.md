# Newcomer's guide to micropython-pydevices

This repository builds MicroPython firmware with PyDevices modules in it. It
holds our patches to one pinned MicroPython release, our variants, and the
build command. It is not a fork, and it publishes no Python package.

## Build something

```bash
./build_mp.py --port unix --variant pydevices --modules displayif,pygraphics
./build_mp.py --port esp32 --board ESP32_GENERIC_S3 --variant SPIRAM_OCT --flash 8MB --modules all
./build_mp.py          # asks for port, board, variant and modules
```

The first run fetches what it needs. In the PyDevices workspace it links the
sibling checkouts instead.

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

## Where things are

| Path | What it is |
|---|---|
| `build_mp.py` | The build command. |
| `UPSTREAM` | The MicroPython release the patches apply to. |
| `modules.lock`, `deps.lock` | What gets fetched when it's missing. |
| `modules/` | The module manifest, `all/`, `castif/`, and the modules. |
| `patches/` | The patch series and `apply_patches.py`. |
| `variants/` | Our unix, windows and webassembly variants. |
| `boards/esp32/` | Board dirs from before the reorg, until step 5 of the plan. |

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
