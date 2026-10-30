# Boards and variants

## Boards we build for

Every board here is one of upstream's, with at most a small variant of ours on
top. Add `--modules` to any of these:

| Board | Command |
|---|---|
| Waveshare ESP32-P4-WIFI6-DEV-KIT, USB device on a USB-C wired to header P6 | `--port esp32 --board ESP32_GENERIC_P4 --variant WIFI6_DEV_KIT --flash 16MB` |
| Waveshare ESP32-P4-WIFI6-DEV-KIT, USB device on its USB-C marked "USB" | `--port esp32 --board ESP32_GENERIC_P4 --variant WIFI6_DEV_KIT_H2 --flash 16MB` |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-4B | `--port esp32 --board ESP32_GENERIC_P4 --variant PRE_REV3_C6_WIFI --flash 32MB` |
| Another ESP32-P4 board, chip revision 3 or later, with an ESP32-C6 for Wi-Fi | `--port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI --flash 16MB` |
| Waveshare ESP32-S3-Touch-LCD-7 | `--port esp32 --board ESP32_GENERIC_S3 --variant LCD_7 --flash 8MB` |
| Waveshare ESP32-S3-Touch-LCD-4.3 | `--port esp32 --board ESP32_GENERIC_S3 --variant LCD_4_3 --flash 8MB` |
| LilyGO T-Embed S3 | `--port esp32 --board ESP32_GENERIC_S3 --variant T_EMBED --flash 16MB` |
| WT32-SC01 Plus | `--port esp32 --board ESP32_GENERIC_S3 --flash 16MB` |
| Raspberry Pi Pico (RP2040) | `--port rp2 --board RPI_PICO` (not `--modules all`: [it doesn't fit](../guides/fit-firmware-on-a-small-flash.md)) |
| Raspberry Pi Pico 2 (RP2350) | `--port rp2 --board RPI_PICO2` |
| Pimoroni Pico Plus 2 W (16 MB flash, 8 MB PSRAM, 48 GPIOs) | `--port rp2 --board RPI_PICO2_W --variant PIMORONI_PICO_PLUS2_W` |

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
| esp32 | `ESP32_GENERIC_S3` `LCD_4_3`, `LCD_7`, `T_EMBED` | The S3 boards in the table above. |
| esp32 | `ESP32_GENERIC_P4` `WIFI6_DEV_KIT`, `WIFI6_DEV_KIT_H2` | The P4 DEV-KIT, with the USB device on the two different connectors in the table above. |
| rp2 | `RPI_PICO2_W` `PIMORONI_PICO_PLUS2_W` | The Pimoroni Pico Plus 2 W. |

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
| `lilygo_twatch_s3_single_app` | The T-Watch S3 with one 4 MB app slot, so LVGL fits. |
| `waveshare_esp32p4_wifi6_dev_kit` | A revision 3 chip, console on USB Serial/JTAG, one 6 MB app slot. |
| `waveshare_esp32p4_wifi6_touch_lcd_4b` | Revision 1, console on the CH343 UART, one 6 MB app slot. Its panel isn't set up. |
