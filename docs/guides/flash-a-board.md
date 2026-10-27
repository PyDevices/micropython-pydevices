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

## With mpftp

mpftp writes either file for you, given the build's port and your board's
serial port:

```bash
mpftp firmware flash --port esp32 --artifact firmware.bin -d /dev/ttyACM0
mpftp firmware flash --port rp2 --uf2 --artifact firmware.uf2
```

For a `.uf2`, mpftp copies it to the board's drive instead.

## After flashing

pydevices' [Bringing up a board](https://github.com/PyDevices/pydevices/blob/main/docs/board-bringup.md)
takes a flashed board to one drawing on its own panel.
