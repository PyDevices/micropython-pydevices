# How it works

This repository builds MicroPython-compatible and CircuitPython-compatible
firmware with the PyDevices C modules compiled in. It holds our patches to one
pinned MicroPython release, our variants and the build command. It is not a
fork, and it publishes no Python package.

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
variant (always optional, as with upstream's `make`), and the modules. A module
is a directory with a `manifest.py`, or a C module with its glue, like ulab.
[Choose modules](../guides/choose-modules.md) says how to name them.

## Why the build tries and then grows

On esp32 and rp2 the build can grow the firmware's share of flash to fit the
image. It finds out by trying: the first link fails, and only then does it
build again with a bigger partition. A saved `sdkconfig` in the build directory
would otherwise pin the wrong partition table for every run after a failed one,
so the build deletes it first and afterwards checks that the configured table is
the one it chose. [Fit firmware on a small flash](../guides/fit-firmware-on-a-small-flash.md)
is the user's side of this.

## Rules

- Upstream is **pinned**. Moving `UPSTREAM` re-validates every patch.
- Patches are justified in their headers. Never hand-edit a patched tree: change
  the patch and prepare again.
- [`build_mp.py`](https://github.com/PyDevices/micropython-pydevices/blob/main/build_mp.py) names no version and no setting. Pins move in the lock files,
  and esp32 settings live in sdkconfig fragments it finds by convention.
- Publishing binaries is a separate decision from this source repository.

## Traps the build already handles

**`BUILD=` and the mpy-cross sub-make.** Passed on its own, `BUILD=` reaches the
mpy-cross sub-make and breaks an esp32 link
([micropython#19667](https://github.com/micropython/micropython/issues/19667)).
Both build systems skip that sub-make when `MICROPY_MPYCROSS` names a built
mpy-cross, so `build_mp.py` builds mpy-cross on its own first and exports its
path.

**Upstream's generic esp32 boards give the app 1.94 MiB**
(`partitions-4MiBplus.csv`), and the kitchen sink is about 4.5 MiB on a P4 and
4.1 MiB on an S3. Autosize handles that, but only if the flash size is right:
gen_esp32part refuses a table larger than the configured flash. Hence `--flash`.

**`make clean` on esp32 is `idf.py fullclean`**, which deletes the whole port's
`managed_components/`. Never run it against a build directory that doesn't exist
yet.

**Two esp32 builds at once in one port race** on `managed_components/`, so
`build_mp.py` builds esp32 one at a time.

## The vst3-engine variants

The `vst3-engine` variants are deliberately narrow. They set `MICROPY_PY_SOCKET`,
`MICROPY_PY_SSL` and `MICROPY_PY_FFI` to 0, so plugin content can't reach the
network or native libraries. A prepared tree carries the Windows networking and
FFI patches regardless; the variant is the guard. Don't swap it for a broader
desktop variant because that one builds.

## More

- [How a CircuitPython build differs](circuitpython.md)
- [How esp-vision's modules are built](esp-vision.md)

```{toctree}
:hidden:

circuitpython
esp-vision
```
