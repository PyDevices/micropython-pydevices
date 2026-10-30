# Install

You build firmware here, so "install" means getting the repository and the
packages your port's compiler needs. Nothing is published to PyPI.

## Get the repository

On Linux, WSL, or Git Bash on Windows:

```bash
curl -fsSL https://pydevices.github.io/install.sh | sh
```

It clones the repository into `micropython-pydevices` in the current directory
and checks your tools. It prints only what your machine is missing, one
`apt-get install` line per port, so you can leave out ports you won't build.
It can install a port's packages for you, with `sh -s -- --deps rp2` (or
`all`) in place of the plain `sh`.

Or clone it yourself:

```bash
git clone https://github.com/PyDevices/micropython-pydevices.git
cd micropython-pydevices
```

Native Windows builds, from PowerShell, are in
[Build on Windows](../guides/build-on-windows.md). To build with nothing
installed at all, use [a codespace](../guides/build-in-a-codespace.md).

## Install what each port needs

[`build_mp.py`](../../build_mp.py) fetches the big pieces itself: MicroPython, the modules, ESP-IDF,
emsdk, the pico-sdk and picotool, and SDL2 for MinGW. What it can't fetch are
your distribution's packages. On Debian or Ubuntu (WSL included):

| Port | Packages |
|---|---|
| every port | `git make cmake python3 gcc g++` |
| unix | `pkg-config libsdl2-dev libffi-dev` (the desktop display) |
| esp32 | `ninja-build python3-venv libusb-1.0-0` (ESP-IDF's installer checks the openocd it fetches, which needs libusb) |
| rp2 | `gcc-arm-none-eabi libnewlib-arm-none-eabi` |
| windows, cross-built from Linux | `gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64 libtool libltdl-dev autoconf automake` (the binutils package has `windres`) |
| webassembly | `nodejs` |

All of it in one line:

```bash
sudo apt-get update && sudo apt-get install -y git make cmake ninja-build \
    python3 python3-venv python3-pip libusb-1.0-0 gcc g++ pkg-config libffi-dev \
    libsdl2-dev gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64 libtool \
    libltdl-dev autoconf automake \
    gcc-arm-none-eabi libnewlib-arm-none-eabi nodejs
```

Other distributions carry the same tools under their own names. A build for a
port that is still missing something stops and says so.

## Room on disk

The first run of each port fetches what it needs. Where the repositories are
already checked out side by side, `build_mp.py` links those sibling checkouts
instead of fetching.

An esp32 build's first run clones ESP-IDF into the clone's `deps/` and
installs its toolchains, for every ESP chip, under `~/.espressif`, outside the
clone (set `IDF_TOOLS_PATH` to put them elsewhere). The toolchains stay put,
but ESP-IDF goes with the clone: move the clone, or clone again, and the next
esp32 build fetches ESP-IDF again. Each build folder takes room in the clone
too.

Next: [build your first firmware](first-program.md).
