# Run your own packages from ROMFS

A board with a ROMFS partition runs your compiled packages straight from flash,
the way it runs the modules frozen into its firmware. Their code never loads
into RAM, so a program that only just fits from `/lib` has room to spare, and
you don't build any firmware: you compile your `.py` files on your computer and
write them to the partition.

The ESP32 Audio Kit's firmware (`ESP32_GENERIC` `AUDIO_KIT`) has a 32 KB ROMFS
partition: its firmware fills the rest of the 4 MB, so that is room for the
kit's own drivers and about 20 KB of yours. Flashing that table moves the
kit's filesystem, so copy your files off first and put them back after.

On the Arduino GIGA R1 the firmware's own Python lives in ROMFS, so
your files go in beside it ([flash a board](flash-a-board.md#stm32-arduino-giga-r1-wifi)).

## Check the board has one

```python
import vfs
print(vfs.rom_ioctl(1))   # how many ROMFS partitions: 1
```

An `AttributeError` means the firmware was built without ROMFS, and 0 means it
has no partition for one. For a board of your own, see
[add a ROMFS partition](#add-a-romfs-partition-to-a-board) below.

## Lay out your files

Make a folder that looks the way the files should import. At boot the board
mounts the partition at `/rom` and puts `/rom` and `/rom/lib` on `sys.path`,
after the frozen modules and before `/lib`:

```text
romfs/
  lib/
    datetime.py
    greenhouse/
      __init__.py
      schedule.py
```

## Compile and deploy

Install `mpremote` and `mpy-cross` on your computer, then deploy the folder:

```bash
pip install mpremote mpy-cross
mpremote connect /dev/ttyUSB0 romfs deploy romfs
```

`romfs deploy` compiles each `.py` to `.mpy` with `mpy-cross`, builds the image,
writes it to the partition and resets the board. The image must fit the
partition: `mpremote romfs query` shows its size. `mpremote romfs build romfs
-o greenhouse.romfs` writes the image to a file instead, and deploying it later
skips the compile.

Use an `mpy-cross` whose bytecode version matches the firmware's. Any release
from 1.23 on writes version 6, which MicroPython 1.23 and later read; a
mismatch fails at import with `ValueError: incompatible .mpy file`.

## Import it

```python
import greenhouse, datetime
print(greenhouse.__file__)   # /rom/lib/greenhouse/__init__.mpy
```

Deploying again replaces the whole partition, so keep the folder as the one
copy of what's on it. On the Audio Kit that folder holds the kit's drivers
(`board_config.py`, `board_peripherals`, `es8388`, `ft6x36`, `ili9341`) in its
`lib` too. A file of the same name in `/lib` loses to the one in
`/rom/lib`: delete it from `/lib`, or deploy without it.

## What it saves

On the Audio Kit, importing `datetime` from micropython-lib (23 KB of Python)
and a two-module package took this much of the heap:

| From | Heap taken | Import time |
|---|---|---|
| `.py` in `/lib` | 25,776 bytes | 567 ms |
| `.mpy` in `/lib` | 15,936 bytes | 113 ms |
| `.mpy` in ROMFS | 8,400 bytes | 36 ms |

From ROMFS only the objects the code makes at import take RAM; the bytecode,
its strings and its constants stay in flash.

## Add a ROMFS partition to a board

This needs one firmware build. On esp32, turn ROMFS on in the variant's
`mpconfigvariant.cmake`:

```cmake
list(APPEND MICROPY_DEF_BOARD MICROPY_VFS_ROM=1)
```

and give its partition table a `romfs` row of subtype `0x8f`, as the Audio Kit's
[`partitions.csv`](https://github.com/PyDevices/micropython-pydevices/blob/main/variants/esp32/ESP32_GENERIC/AUDIO_KIT/partitions.csv)
does. Take the room from the app partition, not the filesystem, and the board
keeps its files when you flash the new table. [Add a board
variant](add-a-board-variant.md) covers the rest.
