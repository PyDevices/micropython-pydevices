# Build your first firmware

This builds MicroPython for your desktop with the pygraphics module compiled
in, then runs a program on it. It needs no board and only the unix packages
from [Install](install.md), and it proves the build works before you spend an
hour on an esp32 one.

From the clone:

```bash
./build_mp.py --port unix --modules pygraphics
```

The first run fetches MicroPython and the module. When it ends it prints where
the interpreter is:

```text
Built into /home/you/micropython-pydevices/builds/unix/standard
  /home/you/micropython-pydevices/builds/unix/standard/micropython
```

Save this as `hello.py`:

```python
import sys
import pygraphics

fb = pygraphics.FrameBuffer(bytearray(160 * 80 * 2), 160, 80, pygraphics.RGB565)
area = fb.round_rect(8, 8, 64, 40, 8, 0xF800, f=True)
print(sys.implementation.name, pygraphics.implementation(), area)
```

Run it with the interpreter you built:

```bash
builds/unix/standard/micropython hello.py
```

```text
micropython native_cmod Area(8, 8, 64, 40)
```

`native_cmod` says pygraphics is the compiled module: the C code you named
with `--modules` is inside the interpreter. Beside the interpreter, the build
folder holds `pydevices-build.json`, a record of every module's commit, so
you can tell later what a firmware was made from.

Now build for a board: [Build for a board](../guides/build-for-a-board.md).
