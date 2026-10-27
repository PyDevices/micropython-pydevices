# Build in a codespace

You can build without installing anything, in a GitHub codespace or in a dev
container on your own machine. Each port has its own container, holding only
that port's packages: unix, webassembly, rp2, windows and esp32.

[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/PyDevices/micropython-pydevices)

## Open one

On GitHub, open the repository's **Code** menu, then **Codespaces**, **...**,
**New with options**, and choose your port under "Dev container
configuration". With VS Code and Docker on your own machine, open your clone
and run **Dev Containers: Reopen in Container**, which asks for the port.

## Build

When the container is ready, its terminal prints the build line for its port,
and you build as you would anywhere else:

```bash
./build_mp.py --port rp2 --board RPI_PICO2 --modules all
```

The first build fetches the port's toolchain into `deps/`, a volume that
outlives the container, so rebuilding the container doesn't fetch it again.
The esp32 container fetches ESP-IDF and its compilers while it's being
created, so your first build starts compiling; it asks for a 4-core machine.

## Get the file out

To flash, take the file to the machine your board is plugged into. In the
Explorer, open `builds/` down to your build, right-click `firmware.bin` (or
`firmware.uf2` on rp2) and choose **Download**. Then write it as
[Flash a board](flash-a-board.md) says.

## Try a webassembly build in workbench

The webassembly container opens your build in workbench, the browser IDE.
Build, then run [`scripts/serve_workbench.sh`](../../scripts/serve_workbench.sh): it downloads the newest workbench
release, puts your `micropython.wasm` in it and serves it on port 8000, which
the container forwards and your browser opens. Click the simulator button (the
display icon) and the REPL that starts is your build.
