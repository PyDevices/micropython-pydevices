# Build for CircuitPython

The same command builds OmniPython CP (CircuitPython-compatible) firmware:
CircuitPython with our C modules compiled in, for CircuitPython's own ports and
boards. Add `--interpreter circuitpython`:

```bash
./build_mp.py --interpreter circuitpython --port raspberrypi \
    --board adafruit_feather_rp2040 --modules pygraphics
./build_mp.py --interpreter circuitpython --port unix --modules pygraphics
```

Ports and boards are CircuitPython's own (`raspberrypi`, `atmel-samd`,
`espressif`, `unix` and the rest), with boards named by their directories
under `ports/<port>/boards/`. Boards CircuitPython has no definition for are
ours: [the boards table](../reference/boards.md#circuitpython-boards).

Output lands in `builds/circuitpython/<port>/<board or variant>/`, with the
same build record. The unix port builds CircuitPython's default variant,
`coverage`, unless you name another.

It takes C modules only. [The modules table](../reference/modules.md) says
which build on CircuitPython and why a module is left out.

## Make arguments

Arguments the script doesn't recognise go to make. `CIRCUITPY_ULAB=0`, for
example, frees flash on a small board:

```bash
./build_mp.py --interpreter circuitpython --port espressif \
    --board adafruit_qtpy_esp32_pico --modules audiodsp CIRCUITPY_ULAB=0
```

They are part of the build record: a build with different ones starts from an
empty build directory, because CircuitPython's generated module table would
otherwise keep the old set.

## Builds from Linux or WSL only

A CircuitPython build runs on Linux or WSL. How it differs from a MicroPython
build is in [How a CircuitPython build differs](../how-it-works/circuitpython.md).
