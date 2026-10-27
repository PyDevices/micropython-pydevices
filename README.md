# micropython-pydevices

micropython-pydevices builds firmware: MicroPython-compatible and
CircuitPython-compatible interpreters with the PyDevices C modules compiled in,
for boards, desktops and the browser, with one command.

```bash
curl -fsSL https://pydevices.github.io/install.sh | sh
```

That clones this repository into `micropython-pydevices` in the current
directory (Linux, WSL, or Git Bash on Windows) and tells you which packages your
machine still needs. Or clone it yourself: `git clone
https://github.com/PyDevices/micropython-pydevices.git`. The build fetches
everything else.

Build MicroPython for your desktop with the pygraphics module inside:

```bash
./build_mp.py --port unix --modules pygraphics
```

Save this as `hello.py`:

```python
import sys
import pygraphics

fb = pygraphics.FrameBuffer(bytearray(160 * 80 * 2), 160, 80, pygraphics.RGB565)
area = fb.round_rect(8, 8, 64, 40, 8, 0xF800, f=True)
print(sys.implementation.name, pygraphics.implementation(), area)
```

```bash
builds/unix/standard/micropython hello.py
```

```text
micropython native_cmod Area(8, 8, 64, 40)
```

For a board, name its port, board and variant, and the modules you want:

```bash
./build_mp.py --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI \
    --flash 16MB --modules displayif,pygraphics
```

[Docs](https://github.com/PyDevices/micropython-pydevices/blob/main/docs/index.md) · [Examples](https://pydevices.github.io/pydevices-examples/gallery/) · [Roadmap](ROADMAP.md)

Made by [PyDevices](https://pydevices.com).
