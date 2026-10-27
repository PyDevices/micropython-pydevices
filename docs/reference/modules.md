# Modules

The short names `--modules` takes. Each is a directory in `modules/`, either
tracked here or linked from a sibling checkout or cloned at the commit in
`modules.lock`. [Choose modules](../guides/choose-modules.md) says how to pick.

## C modules that live here

| Module | What it is |
|---|---|
| [`audioif`](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/audioif) | The audio hardware under the audio pump, with CircuitPython's `audiobusio.I2SOut` and `audioi2sin.I2SIn` on top. |
| [`cameraif`](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/cameraif) | A MIPI-CSI camera as a MicroPython object, on the ESP32-P4. |
| [`castif`](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/castif) | Casts an ESP32-P4's screen and sound to a TV over Wi-Fi Display, or serves them as live HLS. |
| [`displayif`](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/displayif) | The display buses and panels under `displaydev` (SPI, I2C, i80, QSPI, dot-clock RGB, MIPI-DSI, PicoDVI, RGB matrix, SDL2). The dot-clock, MIPI-DSI, PicoDVI and RGB matrix modules keep CircuitPython's names. |
| [`h264enc`](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/h264enc) | RGB565 frames to H.264 on the ESP32-P4's hardware encoder. |
| [`jpegio`](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/jpegio) | JPEG decode and encode, with CircuitPython's `jpegio` API. |
| [`mdns`](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/mdns) | Find and advertise services on the local network, with CircuitPython's `mdns` API (esp32, rp2, unix, windows). |
| [`pngio`](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/pngio) | PNG encode and decode, RGB565 in and out. |
| [`ppa`](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/ppa) | Scale, rotate, convert and fill pixel buffers, on the ESP32-P4's Pixel Processing Accelerator or in portable C elsewhere. |
| [`tsmux`](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/tsmux) | MPEG-TS and HLS segments from H.264. |
| [`usbhost`](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/usbhost) | The USB host port and isochronous streams that `usb.core` and the `usb.host.*` packages are built on (ESP32-S2, S3 and P4). |

## Modules from other repositories

| Module | Where it comes from |
|---|---|
| `audiodsp`, `pygraphics`, `ulab`, `lvgl-micropython` | Cloned at the commit in `modules.lock` when `modules/<name>` is missing, or linked from a sibling checkout. |
| `pydevices`, `palettes`, `pdwidgets`, `audiocomponents` | Manifests here that require the Python packages from omnipython-lib. |
| `micropython-lib` | MicroPython's own upstream packages, frozen from the checkout the MicroPython tree carries. |
| `tflite`, `vision` | [esp-vision's modules](../guides/use-esp-vision-modules.md), opt-in: `all` leaves them out. |

`modules/all` is not a module but the rule behind `all`: every module beside it
except those with an `OPT_IN` file.

## Which modules build on CircuitPython

What `--modules` can name on CircuitPython, and why a module is missing where
it is. "Unix" is CircuitPython's unix port, built and imported in
[`circuitpython.yml`](https://github.com/PyDevices/micropython-pydevices/blob/main/.github/workflows/circuitpython.yml);
"boards" are the five CircuitPython cells of
[`release-firmware.yml`](https://github.com/PyDevices/micropython-pydevices/blob/main/.github/workflows/release-firmware.yml).

| Module | Unix | Boards | Notes |
|---|---|---|---|
| pygraphics | yes | all five | |
| audiodsp | yes | the four ESP32 cells | The modules CircuitPython lacks, `audiograph` and `audiopump.Ring` (Python-made audio that `audiobusio.I2SOut` plays) among them; its audiocore, synthio and the rest stay CircuitPython's. With all of it, a LilyGO T-Embed S3 build fits that board's 2 MB app slot only with `CIRCUITPY_ULAB=0` on the make line. |
| displayif | yes (`usdl2`) | no, on purpose | Boards have CircuitPython's own `displayio` and display buses. pydevices' `SDLDisplay` runs on top of `usdl2`. |
| pngio | yes | all five | Decodes with CircuitPython's older uzlib. |
| tsmux | yes | all five | |
| ppa | yes (software) | ESP32-P4 | The hardware path on the P4. |
| jpegio | yes | ESP32-P4 | Adds only the encoder's C API: CircuitPython has its own `jpegio` decoder. |
| cameraif | yes (`available()` is False) | ESP32-P4 | |
| mdns | yes | no, on purpose | CircuitPython's boards have an `mdns` of their own; its unix port has none. |
| usbhost | yes, as `_usbhost`, with no host engine | ESP32-P4 | The host engine runs on ESP-IDF's USB host library, which from ESP-IDF 6 is Espressif's `usb` component: `build_mp.py` fetches it at the version `deps.lock` pins. The P4's boards leave its high-speed controller to us. Tethering is left out until the port has a network. The S3's one controller is CircuitPython's drive and REPL, so it gets the module without the engine. On the RP2040 and i.MX, CircuitPython's own `usb.core` is the host. |
| audioif | no | no | Links against audiodsp's audio pump and `cp_compat`, which audiodsp's CircuitPython glue doesn't build. It isn't needed there: audioif gives MicroPython the `audiobusio` and `audioi2sin` that CircuitPython boards already have. |
| ulab | CircuitPython's own | CircuitPython's own | |
| castif | no: it is the ESP32-P4's | ESP32-P4, with no network | Talks to lwIP directly, the lwIP under CircuitPython's own `socketpool`. CircuitPython's P4 port has no network until it drives the boards' ESP32-C6, so castif is built without one and `start()` says so. Name tsmux, h264enc and ppa with it. |
| h264enc | no: it is the ESP32-P4's encoder | ESP32-P4 | Espressif's `esp_h264`, which CircuitPython's port, built with the component manager off, can't fetch itself, is pinned in `deps.lock` and fetched by `build_mp.py`. Name ppa with it. |
| tflite, vision | no | no, on purpose | esp-vision's modules, for MicroPython only. |
| lvgl-micropython | no | no | CircuitPython's binding is `lvgl-circuitpython`, passed by path. |
| micropython-lib | no | no | MicroPython's own upstream packages. |
| pydevices, palettes, pdwidgets | frozen | frozen | Python only. On unix, pydevices also freezes its desktop files (the boards drop them), and pdwidgets and palettes with it: together their names pass CircuitPython's 64 KB frozen-string pool, so `build_mp.py` builds the unix port with four-byte qstr offsets (patches 0001 and 0010). Boards keep two-byte offsets. |

What is planned for the rest is in the [roadmap](https://github.com/PyDevices/micropython-pydevices/blob/main/ROADMAP.md).
