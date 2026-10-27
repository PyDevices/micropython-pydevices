# Add a board variant

Most boards need no variant: pick the upstream board that matches the chip and
give `--flash` its flash size ([Build for a board](build-for-a-board.md)). Make
a variant only for settings that really are one board's, such as the LCD-7's
cache sizes and tick.

## An esp32 board

On esp32, `build_mp.py` appends our ESP-IDF settings after the board's, so ours
win. They come from files it finds by name, each used only if it exists, in
this order:

1. `variants/esp32/sdkconfig`, then `variants/esp32/sdkconfig.<chip>`
   (`esp32s3`, `esp32p4`): what every build, or every build for that chip,
   wants. The S3's PSRAM, Wi-Fi and lwIP tuning and the P4's BLE-over-C6
   settings are here.
2. The same two names at the root of each module you selected: what that module
   needs. usbhost's USB host settings and cameraif's OV5647 driver live in
   their own module directories.
3. `variants/esp32/<BOARD>/<VARIANT>/sdkconfig`: what's left that is one board's
   alone.

A variant directory of ours also holds an `mpconfigvariant.cmake` that includes
the upstream variant it builds on, which is how `LCD_7` and `T_EMBED` become
variants of `ESP32_GENERIC_S3`, and `WIFI6_DEV_KIT` one of `ESP32_GENERIC_P4`.

It may also hold:

- An `mpconfigboard.h`, included before the board's own, for C defines the
  port's ESP-IDF components must see too. `WIFI6_DEV_KIT`'s sets
  `MICROPY_HW_USB_HS (0)`, which moves the USB device to the P4's full-speed
  controller, so other P4 boards keep theirs on the high-speed one.
- C sources, through `MICROPY_SOURCE_BOARD` in its `mpconfigvariant.cmake`, the
  way upstream's `ESP32_GENERIC_C2` does. `WIFI6_DEV_KIT_H2` builds on
  `WIFI6_DEV_KIT` and adds a board startup hook that swaps the P4's two
  full-speed PHYs, so the USB device comes out on the kit's USB-C marked "USB".

The partition table is found the same way, the most specific `partitions.csv`
winning ([`variants/esp32/partitions.esp32s3.csv`](https://github.com/PyDevices/micropython-pydevices/blob/main/variants/esp32/partitions.esp32s3.csv)
adds a coredump partition on the S3, which a standard build leaves unused).
Without one, the board's own is used. Either way, autosize grows the app
partition when the image doesn't fit.

## An rp2 board

An rp2 board of ours is a variant of one of upstream's, in
`variants/rp2/<BOARD>/<VARIANT>/`. The port only knows variants that sit in the
board's own folder, so `build_mp.py` makes a folder of its own for the build,
`builds/rp2/<BOARD>/<VARIANT>/<BOARD>/`, links upstream's board into it
unchanged and adds what the variant holds:

- `mpconfigvariant.cmake` (required): cmake settings, included after the
  board's own default variant. The board's `PICO_BOARD`, the number of GPIOs,
  PSRAM and the size of the filesystem go here.
- `mpconfigboard.h`: C defines, included after the board's own header, so it
  can `#undef` what the board sets, such as its name.
- `pins.csv`: the pin names, in place of the board's.

`PIMORONI_PICO_PLUS2_W` is the one rp2 variant of ours. It builds the Pico 2 W for the
Pimoroni board's 16 MB flash, 8 MB PSRAM, 48 GPIOs and its own pin names, and
its files say why each setting is there.

## Desktop variants

The unix and windows ports take our `pydevices` and `vst3-engine` variants, and
webassembly takes `pydevices`. [The variants table](../reference/boards.md#variants)
lists them.
