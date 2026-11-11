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

The `DISPLAY_SHIELD` build makes three images: `firmware0.bin` for the
microcontroller's flash, `firmware1.bin`, the Wi-Fi chip's firmware, for the
QSPI flash, and `romfs.img` for the ROMFS partition beside it. Put the board in
its bootloader, by running `machine.bootloader()` at its REPL or
double-tapping RESET, and write them with
[dfu-util](https://dfu-util.sourceforge.net/), `firmware0.bin` last:

```bash
cd builds/stm32/ARDUINO_GIGA/DISPLAY_SHIELD
dfu-util -a 1 -d 2341:0366 -s 0x90F00000 -D firmware1.bin
dfu-util -a 1 -d 2341:0366 -s 0x90B00000 -D romfs.img
dfu-util -a 0 -d 2341:0366 -s 0x08040000:leave -D firmware0.bin
```

`romfs.img` holds the firmware's own Python modules: asyncio, webrepl, mip,
requests, logging and the rest. The firmware doesn't freeze them, because its
flash is needed for LVGL, so it reads them from the ROMFS, and `firmware0.bin`
without them boots with none of them. The build also leaves them as files in
`romfs/`.

A firmware release carries the same files with the address each is written at
in its name: `...firmware1-at-0x90F00000.bin` is `firmware1.bin`,
`...romfs-at-0x90B00000.img` is `romfs.img` and
`...firmware0-at-0x08040000.bin` is `firmware0.bin`. Use them in the commands
above, and the release notes list the lines with the names filled in. Its
`...romfs-files.zip` is the `romfs/` folder.

The `DISPLAY_SHIELD` firmware runs its USB-C port on TinyUSB, so a program can
add USB interfaces at run time with `machine.USBDevice`, such as a MIDI device
with micropython-lib's `usb-device-midi`. The REPL's serial port and the
`/flash` drive are there as before, but Windows sees a new device the first
time and gives the serial port a new COM number. `machine.bootloader()` still
reaches the DFU bootloader.

`firmware1.bin` holds the radio's firmware for Wi-Fi and for Bluetooth, and
only changes when the Wi-Fi driver does, so after the first time you can leave
it out. A build without the variant keeps the Wi-Fi firmware inside its one
image, `firmware.bin`, which the last command writes in place of
`firmware0.bin`, and freezes its Python modules as upstream does.

The OmniPython libraries don't fit in this firmware beside LVGL, so they go in
the ROMFS partition too, where MicroPython runs them in place and finds them
before `/flash/lib`. A deploy replaces the whole partition, so put the
firmware's modules in the folder you deploy, then the libraries. From a clone
of [pydevices](https://github.com/PyDevices/pydevices), with
[mpremote](https://docs.micropython.org/en/latest/reference/mpremote.html):

```bash
git clone --depth 1 https://github.com/PyDevices/pydevices.git
mkdir romfs
cp -r builds/stm32/ARDUINO_GIGA/DISPLAY_SHIELD/romfs/* romfs/
cp -r pydevices/lib/* romfs/
mpremote romfs deploy romfs
```

From a release, `unzip ...romfs-files.zip -d romfs` takes the place of the
first `cp`. mpremote compiles each `.py` to `.mpy` on the way when the
`mpy-cross` Python package is installed (`pip install mpy-cross`; its 1.29
release writes `.mpy` files this firmware reads). Without it, mpremote copies
the `.py` files and the board compiles them as they're imported. The board's
own drivers go in the same folder:
[the GIGA Display Shield's board page](https://github.com/PyDevices/pydevices/blob/main/docs/boards/display-boards.md#arduino-giga-r1-wifi--giga-display-shield)
has that recipe.

The H747 has a second core, a Cortex-M4, and upstream's layout leaves it the
last 512 KB of flash. A GIGA build that doesn't fit the 1,280 KB before that
takes it too, and says `autosize:` as it does
([fit firmware on a small flash](fit-firmware-on-a-small-flash.md)). You flash
it the same way; `firmware0.bin` is just longer. It overwrites any M4 program
on the board, and it has no `openamp` module, so it can't start one.

### The M4 starts on its own

When a GIGA's BCM4 option bit is set, as it was on the one these builds were
tested on, its M4 starts at reset, before MicroPython runs, from 0x08100000,
the default boot address. To read the bit at the REPL (1 means set; this only
reads it):

```python
import machine
print(machine.mem32[0x5200201C] >> 22 & 1)
```

Any GIGA firmware longer than 768 KB covers that address, upstream's included,
so the M4 starts from whatever bytes of it are there. Usually they point
nowhere it can reach, so it faults and stops. When they point at memory, the M4
can hang the board, so every GIGA build checks them and, if it must, moves the
code a few bytes and links again, saying so in an `m4 pad:` line.

To stop the M4 starting at all, clear BCM4: ST's
[STM32CubeProgrammer](https://www.st.com/en/development-tools/stm32cubeprog.html)
can change it in the option bytes. Changing option bytes is your call, since a
wrong one can leave the board unable to boot until it is recovered over SWD.

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
