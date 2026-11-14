# Boards and variants

## Boards we build for

Every board here is one of upstream's, with at most a small variant of ours on
top. Add `--modules` to any of these:

| Board | Command |
|---|---|
| Waveshare ESP32-P4-WIFI6-DEV-KIT, USB device on a USB-C wired to header P6 | `--port esp32 --board ESP32_GENERIC_P4 --variant WIFI6_DEV_KIT --flash 16MB` |
| Waveshare ESP32-P4-WIFI6-DEV-KIT, USB device on its USB-C marked "USB" | `--port esp32 --board ESP32_GENERIC_P4 --variant WIFI6_DEV_KIT_H2 --flash 16MB`; the released image is built with `--modules all,vision,tflite`, so the wake-word and object-detection programs run on it, in a 9 MB app slot that leaves a 7,104 KB filesystem at 0x910000, 3,072 KB smaller than before, so copy your files off before you first flash one |
| Waveshare ESP32-P4-WIFI6, the Pico-sized board, USB device on header pins 19 (D-) and 20 (D+) | `--port esp32 --board ESP32_GENERIC_P4 --variant WIFI6_DEV_KIT_H2 --flash 32MB`; the release's DEV-KIT image runs on it too (it reads its PSRAM at boot). Write the whole image at 0x2000 as on the DEV-KIT: its filesystem takes the rest of the chip, 23,488 KB at 0x910000 on this board's 32 MB, not the 7,104 KB the release table gives for 16 MB |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-4B | `--port esp32 --board ESP32_GENERIC_P4 --variant PRE_REV3_C6_WIFI --flash 32MB`; its released images now keep a 23,488 KB filesystem at 0x910000, 3,136 KB smaller than before, so copy your files off before you first flash one; they carry `--modules all,vision,tflite`, which needs a 9 MB app slot |
| Espressif ESP32-P4X-EYE | `--port esp32 --board ESP32_GENERIC_P4 --variant P4X_EYE --flash 16MB`; the camera, the H.264 encoder and the detectors come with `--modules all,vision,tflite`, which is the image every release carries |
| Another ESP32-P4 board, chip revision 3 or later, with an ESP32-C6 for Wi-Fi | `--port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI --flash 16MB` |
| Waveshare ESP32-S3-Touch-LCD-7 | `--port esp32 --board ESP32_GENERIC_S3 --variant LCD_7 --flash 8MB` |
| Waveshare ESP32-S3-Touch-LCD-4.3 | `--port esp32 --board ESP32_GENERIC_S3 --variant LCD_4_3 --flash 8MB` |
| LilyGO T-Embed S3 | `--port esp32 --board ESP32_GENERIC_S3 --variant T_EMBED --flash 16MB` |
| Ai-Thinker ESP32-Audio-Kit (ESP32-A1S), CPU at 240 MHz, flash and PSRAM at 80 MHz | `--port esp32 --board ESP32_GENERIC --variant AUDIO_KIT --flash 4MB` (write the bootloader with the app) |
| Adafruit Qualia ESP32-S3, LilyGO T-Display-S3, LilyGO T-HMI: S3 boards with octal PSRAM | `--port esp32 --board ESP32_GENERIC_S3 --variant SPIRAM_OCT --flash 16MB`; every release carries this image |
| Adafruit QT Py ESP32-S3 that has 2 MB of PSRAM, WT32-SC01 Plus, Unexpected Maker FeatherS3 and other S3 boards with quad PSRAM | `--port esp32 --board ESP32_GENERIC_S3 --flash 8MB`; every release carries this image, and a board with more flash runs it too |
| An S3 board with no PSRAM, such as the QT Py ESP32-S3 that has none | `--port esp32 --board ESP32_GENERIC_S3 --variant NO_PSRAM --flash 8MB --modules pydevices,displayif,pngio,palettes,iconkit,svgio,mdns`; every release carries this image |
| Adafruit QT Py ESP32 Pico (8 MB flash, 2 MB PSRAM), CPU at 240 MHz, flash and PSRAM at 80 MHz | `--port esp32 --board ESP32_GENERIC --variant QTPY_PICO --flash 8MB` (write the bootloader with the app) |
| Raspberry Pi Pico (RP2040) | `--port rp2 --board RPI_PICO` (not `--modules all`: [it doesn't fit](../guides/fit-firmware-on-a-small-flash.md)) |
| Raspberry Pi Pico 2 (RP2350) | `--port rp2 --board RPI_PICO2` |
| Pimoroni Pico Plus 2 W (16 MB flash, 8 MB PSRAM, 48 GPIOs) | `--port rp2 --board RPI_PICO2_W --variant PIMORONI_PICO_PLUS2_W` |
| Arduino GIGA R1 WiFi | `--port stm32 --board ARDUINO_GIGA` (past 1,280 KB it [takes the M4 core's flash](../guides/fit-firmware-on-a-small-flash.md)) |
| Arduino GIGA R1 WiFi, playing through its headphone jack ([`audioio.AudioOut`](https://github.com/PyDevices/micropython-pydevices/blob/main/modules/audioif/README.md#audioioaudioout--the-same-output-on-a-dac)) | `--port stm32 --board ARDUINO_GIGA --modules audiodsp,audioif` |
| Arduino GIGA R1 WiFi with the GIGA Display Shield | `--port stm32 --board ARDUINO_GIGA --variant DISPLAY_SHIELD --modules displayif,lvgl` ([three images to flash](../guides/flash-a-board.md#stm32-arduino-giga-r1-wifi)) |

Every ESP32 board here runs its CPU at the chip's rated clock (240 MHz on an
ESP32 or S3; 400 on a P4 of revision 3 or later, 360 before that), and its
flash and PSRAM at the fastest clock its chips are rated for: 80 MHz or more,
except the flash of a P4 image for revisions before 3, which ESP-IDF holds to
40 MHz. [Add a board variant](../guides/add-a-board-variant.md#an-esp32-board)
says how a new board does the same.

Every rp2 board runs its CPU at the chip's rated clock too: 200 MHz on an
RP2040, where the SDK's default is 125, and 150 on an RP2350.

Our ESP32-S3 builds keep the TLS library in PSRAM, and on an S3 without it,
Wi-Fi can't join a WPA2 network. Most S3 boards have PSRAM. For one that
doesn't, build the `NO_PSRAM` variant, which keeps TLS in internal RAM. It
leaves out LVGL and the other modules that want megabytes of heap, so name the
modules you need.

Adafruit sells the QT Py ESP32-S3 with and without PSRAM, and the two need
different images. To tell which you have, plug the board in and run
`esptool chip-id`: a board with PSRAM has an `Embedded PSRAM` entry on its
Features line, and one without has none. With it, take the quad-PSRAM image
(`ESP32_GENERIC_S3`, no variant); without it, the `NO_PSRAM` image. The
quad-PSRAM image boots on a board with none, but the board can't join a WPA2
network.

For a board that isn't listed, see [Build for a board](../guides/build-for-a-board.md#a-board-that-isnt-in-the-table).

## Variants

Our variants, in [`variants/`](https://github.com/PyDevices/micropython-pydevices/tree/main/variants).

| Port | Variant | What it is |
|---|---|---|
| unix | `pydevices` | Upstream's standard variant plus `os.dupterm`, freezing the modules you named and the desktop board config. |
| unix | `vst3-engine` | The interpreter inside the micropython-vst3 plug-in: no sockets, no SSL, no FFI. |
| windows | `pydevices` | The same as unix `pydevices`, with the Windows display and audio backends (`displaydev.windisplay`, `audiodev.win_audio`). |
| windows | `vst3-engine` | The same as unix `vst3-engine`. |
| webassembly | `pydevices` | The browser runtime, with the `wasmbridge` module and a Fetch-backed `requests`. |
| esp32 | `ESP32_GENERIC` `AUDIO_KIT` | The ESP32 Audio Kit: upstream's PSRAM variant with flash and PSRAM at 80 MHz, which keeps Bluetooth call audio whole while a program is busy, and the CPU at 240 MHz, which lets a 48 kHz synth run with its reverb. |
| esp32 | `ESP32_GENERIC_S3` `NO_PSRAM` | An S3 with no PSRAM: upstream's generic build without its PSRAM settings, TLS in internal RAM, and the caches and Wi-Fi buffers at sizes internal RAM can spare. |
| esp32 | `ESP32_GENERIC_S3` `LCD_4_3`, `LCD_7`, `T_EMBED` | The S3 boards in the table above. |
| esp32 | `ESP32_GENERIC_P4` `WIFI6_DEV_KIT`, `WIFI6_DEV_KIT_H2` | The P4 DEV-KIT, with the USB device on the two different connectors in the table above. The Pico-sized ESP32-P4-WIFI6 has the same C6 wiring, and `WIFI6_DEV_KIT_H2` puts its USB device on header pins 19 and 20. |
| esp32 | `ESP32_GENERIC_P4` `P4X_EYE` | The ESP32-P4X-EYE: `C6_WIFI` with ESP-Hosted on the pins its C6 uses, the USB device on the port marked "USB 2.0" and the REPL on "Debug", and a 9 MB app slot for the camera and vision modules. |
| esp32 | `ESP32_GENERIC` `QTPY_PICO` | The QT Py ESP32 Pico: upstream's PSRAM variant with flash and PSRAM at 80 MHz, which keeps Bluetooth audio from falling behind, and the CPU at 240 MHz. |
| rp2 | `RPI_PICO2_W` `PIMORONI_PICO_PLUS2_W` | The Pimoroni Pico Plus 2 W. |
| stm32 | `ARDUINO_GIGA` `DISPLAY_SHIELD` | The GIGA R1 WiFi with room for LVGL and the shield's MIPI DSI panel: the Wi-Fi and Bluetooth firmware moved to QSPI flash as a second image, its Python modules in a ROMFS image instead of frozen, Bluetooth LE through NimBLE, a one-dimensional ulab with numpy's array core and elementwise maths only ([what's out](https://github.com/PyDevices/micropython-pydevices/blob/main/variants/stm32/ARDUINO_GIGA/DISPLAY_SHIELD/ulab_config.h)), LVGL's 40 px font left out, and USB through TinyUSB: `machine.USBDevice` on the USB-C port and a USB host on the USB-A port, where bus-powered devices that draw more than about 500 mA may not run; use a powered hub. |

The esp32 [`variants/esp32/`](https://github.com/PyDevices/micropython-pydevices/tree/main/variants/esp32) folder also holds the `sdkconfig` fragments and
partition table every esp32 or every chip gets:
[Add a board variant](../guides/add-a-board-variant.md) says how they combine.

## CircuitPython boards

With `--interpreter circuitpython`, boards are CircuitPython's own. These are
ours, in [`boards/circuitpython/`](https://github.com/PyDevices/micropython-pydevices/tree/main/boards/circuitpython), for boards CircuitPython has no definition for:

| Board | What it is |
|---|---|
| `feather_rp2040_lvgl` | A Feather RP2040 with room for LVGL. |
| `waveshare_esp32s3_touch_lcd_7` | The 800x480 panel as `board.DISPLAY`, one 4 MB app slot. |
| `waveshare_esp32s3_touch_lcd_43` | The same 800x480 panel as `board.DISPLAY` and one 4 MB app slot, like the LCD-7. |
| `adafruit_qualia_s3_rgb666_single_app` | The Adafruit Qualia ESP32-S3 for RGB666 displays with one 4 MB app slot and no UF2 bootloader, so LVGL fits; flash it with esptool. |
| `lilygo_twatch_s3_single_app` | The T-Watch S3 with one 4 MB app slot, so LVGL fits. |
| `waveshare_esp32p4_wifi6_dev_kit` | A revision 3 chip, console on USB Serial/JTAG, one 6 MB app slot. |
| `waveshare_esp32p4_wifi6_touch_lcd_4b` | Revision 1, console on the CH343 UART, one 6 MB app slot. Its panel isn't set up. |
