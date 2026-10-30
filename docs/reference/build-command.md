# The build command

```text
./build_mp.py [--interpreter {micropython,circuitpython}] [--port PORT]
              [--board BOARD] [--variant VARIANT] [--modules MODULES]
              [--flash FLASH] [--no-autosize] [--coredump] [--clean]
              [make arguments]
```

Leave out `--port` and it asks for port, board and variant. On a port that has
boards, leave out `--board` and it asks for that. `--variant` is always
optional. Leave out `--modules` and it lists [`modules/`](https://github.com/PyDevices/micropython-pydevices/tree/main/modules) and asks.

## Options

| Option | Meaning |
|---|---|
| `--interpreter` | `micropython` (the default) or `circuitpython`: CircuitPython-compatible firmware from CircuitPython's own ports and boards. |
| `--port` | The port: `unix`, `windows`, `webassembly`, `esp32`, `rp2`. With `--interpreter circuitpython`, CircuitPython's own: `espressif`, `raspberrypi`, `unix` and the rest. |
| `--board` | The board, by upstream's directory name. |
| `--variant` | A variant of the board or port: upstream's, or one of ours in [`variants/`](https://github.com/PyDevices/micropython-pydevices/tree/main/variants). |
| `--modules` | A comma list of short names, full paths, or `all`. See [Modules](modules.md). |
| `--flash` | esp32 flash size, such as `16MB`. |
| `--no-autosize` | esp32 and rp2: refuse instead of growing the firmware's share of flash. |
| `--coredump` | esp32: a troubleshooting build that writes a core dump to flash on a crash. Standard builds don't. |
| `--clean` | Delete this target's build directory first. |

Arguments it doesn't recognise go to make, such as `-j4` or `V=1`. On a
CircuitPython build they are part of the build record.

## Environment

| Variable | Default | Meaning |
|---|---|---|
| `OUT_DIR` | `builds/` | Where builds go. |
| `VARIANTS_DIR` | [`variants/`](https://github.com/PyDevices/micropython-pydevices/tree/main/variants) | Where our variants are found. |
| `MODULES_DIR` | [`modules/`](https://github.com/PyDevices/micropython-pydevices/tree/main/modules) | Where modules are found. |
| `JOBS` | every core | The parallelism handed to make. |
| `MSYS2_ROOT` | `C:\msys64` | On Windows, the MSYS2 whose make and MinGW gcc build the windows port. |
| `IDF_TOOLS_PATH` | `~/.espressif` | Where an esp32 build installs ESP-IDF's toolchains. |

## Output

Output lands in `builds/<port>/[<board>/]<variant>/`, and in
`builds/circuitpython/<port>/<board or variant>/` for CircuitPython. A
`--coredump` build adds `-coredump` to the variant's name. With no `--variant`
the last directory is:

| Port | Directory |
|---|---|
| esp32, rp2 | `default` |
| unix, windows, webassembly | `standard`, upstream's default variant |
| CircuitPython unix | `coverage`, CircuitPython's default variant |

Each build folder holds `pydevices-build.json`, a record of every module's
commit, and the files the build made: `firmware.bin` on esp32, `firmware.uf2`
on rp2, `micropython` on unix, `micropython.exe` on windows, `micropython.mjs`
and `micropython.wasm` on webassembly.
