# Build for a board

Every board here is one of upstream's, with at most a small variant of ours on
top. Find yours in [the boards table](../reference/boards.md), which gives the
`--port`, `--board`, `--variant` and `--flash` to use, and add `--modules`:

```bash
./build_mp.py --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI \
    --flash 16MB --modules displayif,pygraphics,/home/you/earful
```

You name the modules you want: ours by short name, anything else by its path,
or `all`. [Choose modules](choose-modules.md) says what each name brings. Leave
out `--port` and the script asks for port, board, variant, flash size and
modules.

Output lands in `builds/<port>/[<board>/]<variant>/`, beside
`pydevices-build.json`, a record of every module's commit. With no
`--variant`, that last directory is `default` on esp32 and rp2, and `standard`
on the desktop ports. [Flash a board](flash-a-board.md) says where
`firmware.bin` goes.

## A board that isn't in the table

A new board usually needs no file at all. Pick the upstream board and variant
that match its chip and PSRAM, and give `--flash` its flash size. Make a
variant of your own only for settings that really are that board's:
[Add a board variant](add-a-board-variant.md).

## When the image doesn't fit

On esp32 and rp2 the build can grow the firmware's share of flash to fit the
image, which moves the filesystem. [Fit firmware on a small flash](fit-firmware-on-a-small-flash.md)
says when that happens and how to stop it.

## When the first link fails

A first link that fails on size, in red, is expected on esp32 and rp2 with
`--modules all`: the build grows the partition and builds again. It, and the
other output that looks wrong and isn't, is in
[Read build warnings](read-build-warnings.md).
