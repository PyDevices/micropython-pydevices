# Files and pins

| Path | What it is |
|---|---|
| [`build_mp.py`](../../build_mp.py) | The build command: [every option](build-command.md). It fills in `micropython/`, [`modules/`](../../modules) and `deps/` if they're missing, prepares the MicroPython tree and runs the port's own `make`. |
| [`UPSTREAM`](../../UPSTREAM) | The MicroPython commit everything applies to. |
| [`modules.lock`](../../modules.lock) | The commits [`build_mp.py`](../../build_mp.py) fetches when a module isn't there. It also pins [omnipython-lvgl](https://github.com/PyDevices/omnipython-lvgl), LVGL with its bindings and a port per interpreter: a build with `lvgl` (or `all`) fetches it into `modules/.omnipython-lvgl` and compiles the port for its interpreter. Beside sibling checkouts, these are links to those checkouts instead. |
| [`deps.lock`](../../deps.lock) | The toolchains [`build_mp.py`](../../build_mp.py) fetches: ESP-IDF, emsdk, SDL2 for MinGW, the TinyUSB the esp32 port builds on, and the ESP-IDF components CircuitPython builds ask for. It also pins CircuitPython, for `--interpreter circuitpython`. |
| [`provenance.json`](../../provenance.json) | The MicroPython pin and the checksum of every patch in [`patches/micropython/`](../../patches/micropython) and [`patches/tinyusb/`](../../patches/tinyusb). |
| [`modules/`](../../modules) | [`manifest.py`](../../modules/manifest.py), the manifest every build freezes; `all/`; the modules, [linked or cloned](modules.md); and `.esp-vision/`, the pin for [esp-vision's modules](../how-it-works/esp-vision.md). |
| [`patches/micropython/`](../../patches/micropython) | Our patch series, `0001-…` to `0042-…` (there is no `0026`, whose fix moved into [`patches/tinyusb/`](../../patches/tinyusb)), each justified in its own header. |
| [`patches/tinyusb/`](../../patches/tinyusb) | Patches to the TinyUSB the esp32 port builds on the S2, S3 and P4, which [`build_mp.py`](../../build_mp.py) applies to `deps/espressif__tinyusb` (at its `deps.lock` tag) and builds in place of the port's own copy. |
| [`patches/micropython-lib/`](../../patches/micropython-lib) | micropython-lib's own fixes from after the commit MicroPython's `lib/micropython-lib` points at. A firmware that freezes those packages, and a board whose `/lib` you fill from the submodule, both get them. |
| [`patches/circuitpython/`](../../patches/circuitpython) | Our patches to CircuitPython, applied to `deps/circuitpython`. |
| [`patches/apply_patches.py`](../../patches/apply_patches.py) | Applies the MicroPython series, plus each module's own MicroPython patches, once, as one local commit. `--check` tests a change without applying it. |
| [`variants/`](boards.md#variants) | Our variants and the esp32 settings. |
| [`boards/circuitpython/`](boards.md#circuitpython-boards) | Boards CircuitPython has no definition for. |
| [`scripts/`](../../scripts) | `exercise_build.sh`, `release_firmware.py`, `stage_firmware_page.py`, `serve_workbench.sh`, `freeze_list.py`, `update_provenance.py`. [Contributing](../../CONTRIBUTING.md) says what each is for. |
