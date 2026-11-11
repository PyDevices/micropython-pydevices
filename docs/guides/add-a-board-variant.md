# Add a board variant

Most boards need no variant: pick the upstream board that matches the chip and
give `--flash` its flash size ([Build for a board](build-for-a-board.md)). Make
a variant only for settings that really are one board's, such as the LCD-7's
cache sizes and tick.

## An esp32 board

On esp32, [`build_mp.py`](https://github.com/PyDevices/micropython-pydevices/blob/main/build_mp.py) appends our ESP-IDF settings after the board's, so ours
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
partition when the image doesn't fit, unless the build keeps a board's
filesystem with `--keep-fs`.

Run the CPU, flash and PSRAM at the fastest clocks the board's chips are
rated for. The CPU's rating is the chip's: 240 MHz for an ESP32, S2, S3 or C5,
160 for a C3 or C6, 400 for a P4 of revision 3 or later and 360 before that.
Every S3 build already runs at 240
([`variants/esp32/sdkconfig.esp32s3`](https://github.com/PyDevices/micropython-pydevices/blob/main/variants/esp32/sdkconfig.esp32s3)),
and a P4 build runs at its revision's rating by default. A classic ESP32
board sets 240 in the variant's `sdkconfig`, as
[`AUDIO_KIT`'s](https://github.com/PyDevices/micropython-pydevices/blob/main/variants/esp32/ESP32_GENERIC/AUDIO_KIT/sdkconfig)
does: upstream's default is 160 MHz, and at 160 a 48 kHz synth with a reverb
runs out of time and clicks. The ESP32-S0WD and ESP32-D2WD are the exceptions,
rated for 160.

Flash and PSRAM are usually rated for 80 MHz, which the variant's `sdkconfig`
sets in two lines
([`QTPY_PICO`'s](https://github.com/PyDevices/micropython-pydevices/blob/main/variants/esp32/ESP32_GENERIC/QTPY_PICO/sdkconfig)).
Upstream's generic boards run them at 40 MHz, and at that clock a classic
ESP32 can fall behind in Bluetooth audio. Read the flash's JEDEC ID with
`esptool.py flash-id` and check its datasheet first. The flash clock lives in
the bootloader, so write the bootloader with the app. A P4 image for chips
before revision 3 keeps its flash at 40 MHz, the most ESP-IDF offers there.

A build with the CPU below its chip's rating, or flash or PSRAM below 80 MHz,
says so when it finishes, with the lines to set.

## An rp2 board

An rp2 board of ours is a variant of one of upstream's, in
`variants/rp2/<BOARD>/<VARIANT>/`. The port only knows variants that sit in the
board's own folder, so `build_mp.py` makes a folder of its own for every rp2
build, `builds/rp2/<BOARD>/<VARIANT>/<BOARD>/`, links upstream's board into it
unchanged, adds
[`variants/rp2/board.cmake`](https://github.com/PyDevices/micropython-pydevices/blob/main/variants/rp2/board.cmake)
after the board's own settings, and adds what the variant holds:

- `mpconfigvariant.cmake` (required): cmake settings, included after the
  board's own default variant. The board's `PICO_BOARD`, the number of GPIOs,
  PSRAM and the size of the filesystem go here.
- `mpconfigboard.h`: C defines, included after the board's own header, so it
  can `#undef` what the board sets, such as its name.
- `pins.csv`: the pin names, in place of the board's.

`PIMORONI_PICO_PLUS2_W` is the one rp2 variant of ours. It builds the Pico 2 W for the
Pimoroni board's 16 MB flash, 8 MB PSRAM, 48 GPIOs and its own pin names, and
its files say why each setting is there.

Every rp2 build runs the CPU at its chip's rated clock: 200 MHz on an RP2040,
which `board.cmake` sets where the SDK's default is still 125, and 150 on an
RP2350. At 200 MHz the SDK raises the RP2040's core voltage to the 1.15 V the
rating needs. Flash and PSRAM follow the CPU through a divider and stay inside
their rating, so a variant has nothing to set for them. A build whose CPU
runs below its chip's rating says so when it finishes.

## An stm32 board

An stm32 variant of ours works the same way, in
`variants/stm32/<BOARD>/<VARIANT>/`, with `make` files instead of cmake ones.
`build_mp.py` makes `builds/stm32/<BOARD>/<VARIANT>/<BOARD>/` from upstream's
board, unchanged, and adds:

- `mpconfigvariant.mk` (required): make settings, included after the board's
  `mpconfigboard.mk`, so it can change what the board sets.
- A header named like one of the board's (`mpconfigboard.h`,
  `stm32h7xx_hal_conf.h`): included after the board's own.
- Any other file, such as a linker script fragment: put beside the board's
  files, where the variant's settings reach it as `$(BOARD_DIR)/<name>`.

`ARDUINO_GIGA`'s `DISPLAY_SHIELD` is the one stm32 variant of ours. It moves
the Wi-Fi chip's firmware into QSPI flash with a linker script fragment and
turns on the HAL's DSI driver for displayif's `mipidsi`. Its `manifest.py`
freezes no Python: it hands its modules to `build_mp.py`, which compiles them
into `romfs.img` for the board's ROMFS partition. Its
`mpconfigboard.h` also switches USB to TinyUSB, which is one line:
`#define MICROPY_HW_TINYUSB_STACK (1)`.

On `ARDUINO_GIGA`, with a variant or without one, a firmware too big for its
1,280 KB grows over the M4 core's flash instead of failing, and the build adds
the settings for that to the variant file it generates. A variant doesn't need
to make room itself; see
[fit firmware on a small flash](fit-firmware-on-a-small-flash.md).

## Desktop variants

The unix and windows ports take our `pydevices` and `vst3-engine` variants, and
webassembly takes `pydevices`. [The variants table](../reference/boards.md#variants)
lists them.
