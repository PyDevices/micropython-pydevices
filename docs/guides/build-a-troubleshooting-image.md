# Build a troubleshooting image with a core dump

Standard builds, including every image on the download page, don't write a core
dump to flash when the firmware crashes. When you need one, to find out why a
board resets or hangs, add `--coredump` on esp32:

```bash
./build_mp.py --port esp32 --board ESP32_GENERIC_S3 --variant SPIRAM_OCT --flash 8MB --modules all --coredump
```

It builds into its own folder, `builds/esp32/<board>/<variant>-coredump/`, so it
never replaces your standard image.

## Read the dump

After a crash the board keeps the dump in its `coredump` partition across the
reboot. Read it back with `esptool` and `esp-coredump`
(`esp_coredump info_corefile`) against that build's `micropython.elf`. The
decode only matches the ELF of the same commit and options, so build the
troubleshooting image from the commit your board runs.

## Which boards can do it

The dump needs a `coredump` partition, and only the ESP32-S3 table has one. A
standard S3 build leaves it unused, which is why the filesystem sits where it
always did, and a troubleshooting build uses the same table: you can flash its
app image (`micropython.bin`, at the app offset) onto a board running a
standard image and keep the files on it.

On a board whose table has no such partition, such as the ESP32-P4,
`--coredump` stops with "this board's table has no coredump partition yet"
instead of moving your filesystem.
