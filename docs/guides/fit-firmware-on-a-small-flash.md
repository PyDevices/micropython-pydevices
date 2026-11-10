# Fit firmware on a small flash

On esp32 and rp2 the build can grow the firmware's share of flash to fit the
image: on esp32 the app partition, on rp2 the region before the filesystem. It
leaves 1/32 of the flash as headroom, 64 KB to 256 KB. That moves the
filesystem, so a board flashed with that image comes up with an empty one.

On the Arduino GIGA R1 the firmware has 1,280 KB, and the 512 KB after it is
the M4 core's. A build that doesn't fit takes that too, 1,792 KB in all, and
leaves out OpenAMP, which loads and talks to M4 programs: with its flash taken,
the board has nowhere to keep one. The filesystem doesn't move. The [flash
guide](flash-a-board.md#stm32-arduino-giga-r1-wifi) says what that means for
the M4.

To refuse the growth and stop instead, add `--no-autosize`.

## Keep a board's files through an update

Autosize works the firmware's share out again in each new build folder, so a
newer image that is a few KB bigger can come out with a smaller filesystem
than the one already on your board, and flashing it empties the board. To keep
the board's filesystem where it is, ask the board its size. On rp2:

```python
import rp2
f = rp2.Flash()
print(f.ioctl(4, 0) * f.ioctl(5, 0))  # 589824 is 576 KB
```

On esp32:

```python
import esp32
print(esp32.Partition.find(esp32.Partition.TYPE_DATA, label="vfs")[0].info()[3])  # 2752512 is 2688 KB
```

and build with that size:

```bash
./build_mp.py --port rp2 --board RPI_PICO --modules displayif,lvgl,pydevices --keep-fs 576K
./build_mp.py --port esp32 --board ESP32_GENERIC_S3 --variant LCD_7 --flash 8MB --modules all --keep-fs 2688K
```

If the firmware fits beside it, the image keeps that filesystem and your files
are there after the flash. If it doesn't, the build stops and says what size
would fit, rather than moving it. On esp32 the filesystem is the end of the
flash, so `--keep-fs` needs `--flash`, and the app partition takes the flash
before it. On rp2, a rebuild in the same build folder starts from the size that
folder used last, so it keeps the filesystem without `--keep-fs` unless the
image has outgrown it.

The firmware page's MicroPython images are built this way, so updating a board
from one release to the next keeps its files. A release that has to move a
board's filesystem says so at the top of its notes.

On esp32, autosize works only if the flash size is right: the partition table
tool refuses a table larger than the configured flash, so give `--flash`.

## What you will see

The build finds out by trying. The first link fails, loudly, and only then does
it build again. On esp32 that is a red `Error: app partition is too small for
binary micropython.bin size 0x…`, then `ninja: build stopped`, `make: *** Error
1` and a Build-Troubleshooting link. On rp2 it is ``region `FLASH' overflowed
by … bytes``, `collect2: error: ld returned 1 exit status` and nested `Error 2`
lines; on the GIGA, ``region `FLASH_TEXT' overflowed by … bytes``. Then an
`autosize:` line says the image didn't fit and the build is running once more.
On the GIGA that second build starts from scratch, because leaving out OpenAMP
changes what every file is compiled with; a later build of the same modules
starts in the larger region. With `all`, expect that second pass on every esp32
and rp2 board; it roughly doubles the build time, because the whole esp32
compile runs twice.

## When nothing will fit

How much fits depends on the flash. On a Pico 2 (4 MB), `all` builds and fits,
using 94 % of its 3392 KB firmware region and leaving a 704 KB filesystem. On
a Pico (2 MB) it can't: the image is bigger than the whole flash, so the build
stops at the first link with the overflow error. Name what your project uses instead:

```bash
./build_mp.py --port rp2 --board RPI_PICO --modules displayif,jpegio,pygraphics,palettes,pdwidgets
```

A display project's set like this one builds to about 540 KB and fits with the
Pico's default 1408 KB filesystem untouched.

On a 4 MB ESP32, `audioinstruments` is the biggest of the modules, and you can
freeze only the instruments you play, and of `audioeffects` only the effects you
use: [Build with a few instruments and effects](build-with-a-few-instruments.md).
