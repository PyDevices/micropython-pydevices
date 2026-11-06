# Flash a board

## ESP32

An esp32 build ends with ESP-IDF's `To flash, run: idf.py flash`. That isn't
the route here: `idf.py` lives in the ESP-IDF environment the build sets up,
not on your shell's `PATH`. Write `firmware.bin` with esptool (`pip install
esptool`) instead. It's the merged image, with bootloader, partition table and
app in one, so it goes where the bootloader goes: 0x2000 on the P4, 0x0 on the
S3. For another chip, the build folder's `flasher_args.json` has the
bootloader's offset.

```bash
esptool --chip esp32p4 write-flash 0x2000 builds/esp32/ESP32_GENERIC_P4/WIFI6_DEV_KIT_H2/firmware.bin
esptool --chip esp32s3 write-flash 0x0 builds/esp32/ESP32_GENERIC_S3/SPIRAM_OCT/firmware.bin
```

## RP2

Hold BOOTSEL while you plug the board in, and copy `firmware.uf2` to the drive
it shows.

## STM32 (Arduino GIGA R1 WiFi)

The `DISPLAY_SHIELD` build makes two images: `firmware0.bin` for the
microcontroller's flash and `firmware1.bin`, the Wi-Fi chip's firmware, for
the QSPI flash. Put the board in its bootloader, by running
`machine.bootloader()` at its REPL or double-tapping RESET, and write both with
[dfu-util](https://dfu-util.sourceforge.net/), the Wi-Fi firmware first:

```bash
cd builds/stm32/ARDUINO_GIGA/DISPLAY_SHIELD
dfu-util -a 1 -d 2341:0366 -s 0x90F00000 -D firmware1.bin
dfu-util -a 0 -d 2341:0366 -s 0x08040000:leave -D firmware0.bin
```

`firmware1.bin` only changes when the Wi-Fi driver does, so after the first
time you can write `firmware0.bin` alone.

PyDevices' Python libraries don't fit in the firmware beside LVGL. Put them in
the board's ROMFS partition instead, which MicroPython searches before
`/flash/lib`: copy the `.mpy` files into a `lib` folder and deploy its parent
with `mpremote romfs deploy <folder>`.

## With mpftp

mpftp writes either file for you, given the build's port and your board's
serial port:

```bash
mpftp firmware flash --port esp32 --artifact firmware.bin -d /dev/ttyACM0
mpftp firmware flash --port rp2 --uf2 --artifact firmware.uf2
```

For a `.uf2`, mpftp copies it to the board's drive instead.

## After flashing

pydevices' [Bringing up a board](https://github.com/PyDevices/pydevices/blob/main/docs/guides/bring-up-a-board.md)
takes a flashed board to one drawing on its own panel.
