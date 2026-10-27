# Fit firmware on a small flash

On esp32 and rp2 the build can grow the firmware's share of flash to fit the
image: on esp32 the app partition, on rp2 the region before the filesystem. It
leaves 1/32 of the flash as headroom, 64 KB to 256 KB. That moves the
filesystem, so a board flashed with that image comes up with an empty one.

To refuse the growth and stop instead, add `--no-autosize`.

On esp32, autosize works only if the flash size is right: the partition table
tool refuses a table larger than the configured flash, so give `--flash`.

## What you will see

The build finds out by trying. The first link fails, loudly, and only then does
it build again. On esp32 that is a red `Error: app partition is too small for
binary micropython.bin size 0x…`, then `ninja: build stopped`, `make: ***
Error 1` and a Build-Troubleshooting link. On rp2 it is ``region `FLASH'
overflowed by … bytes``, `collect2: error: ld returned 1 exit status` and
nested `Error 2` lines. Then an `autosize:` line says the image didn't fit and
the build is running once more. With `all`, expect that second pass on every
esp32 and rp2 board; it roughly doubles the build time, because the whole esp32
compile runs twice.

## When nothing will fit

How much fits depends on the flash. On a Pico 2 (4 MB), `all` builds and fits,
using 93 % of its 2560 KB firmware region and leaving a 1536 KB filesystem. On
a Pico (2 MB) it can't: the image is bigger than the whole flash, so the build
stops at the first link with the overflow error. Name what your project uses instead:

```bash
./build_mp.py --port rp2 --board RPI_PICO --modules displayif,jpegio,pygraphics,palettes,pdwidgets
```

A display project's set like this one builds to about 540 KB and fits with the
Pico's default 1408 KB filesystem untouched.
