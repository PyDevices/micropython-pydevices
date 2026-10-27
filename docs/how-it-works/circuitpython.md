# How a CircuitPython build differs

`--interpreter circuitpython` is a flag rather than a second command because
everything around the interpreter is shared: the module names and
`modules.lock`, the workspace links, the output layout and the build record.
What differs is one function, and these are its parts.

- **The checkout.** CircuitPython is pinned in `deps.lock` like a toolchain and
  cloned into `deps/circuitpython`, always our own clone, never a link to a
  sibling checkout. A build fetches only the submodules its target needs, with
  CircuitPython's own `tools/ci_fetch_deps.py` (a board's name, or `tests` for
  unix), and its build tools go in `deps/circuitpython-venv` from its
  `requirements-dev.txt`.
- **The modules go in as `USER_C_MODULES`**, not a manifest. CircuitPython's
  `py/py.mk` carries MicroPython's `USER_C_MODULES` block unchanged, so a
  module's `micropython.mk` is the one our MicroPython builds read. CircuitPython
  freezes Python its own way (`FROZEN_MPY_DIRS`), so a module with no
  `micropython.mk` and no Python to freeze is refused rather than left out
  quietly. Each module is handed to make through a link named for it, in
  `usermods/` in the build directory: make puts a module's objects under a
  directory named like the module's, and two worktrees checked out under the
  same branch name would otherwise share one.
- **espressif brings its own ESP-IDF.** CircuitPython's espressif port builds
  with the ESP-IDF in `ports/espressif/esp-idf`, not the `esp-idf` in
  `deps.lock`. Before make, the build initialises that tree's submodules
  shallowly (ESP-IDF's CMake would otherwise clone them with their whole
  history), runs its `install.sh` for the board's chip (compilers go in
  `~/.espressif`, shared with any ESP-IDF of the same version) and puts ESP-IDF's
  Python requirements in `deps/circuitpython-venv`.
- **Our patches and boards go on first.** `patches/circuitpython/*.patch` are
  applied to `deps/circuitpython` and each `boards/circuitpython/<port>/<board>`
  is linked into the port's `boards/`, once each; the build record lists the
  patches. [The boards table](../reference/boards.md#circuitpython-boards) lists
  the boards.
- **A module can ask espressif for ESP-IDF components.** CircuitPython's
  espressif port builds and links a fixed list of ESP-IDF components, which
  leaves out the camera's. With patch
  `0006-espressif-esp-idf-components-a-user-c-module-asks-for.patch`, a module's
  `micropython.mk` adds names to `USER_ESP_IDF_COMPONENTS` (cameraif: the CSI
  controller, the ISP and the sensor drivers; jpegio and ppa: the P4's JPEG
  engine and PPA; h264enc: `esp_h264`), and the make line's
  `USER_ESP_IDF_COMPONENT_DIRS` adds components ESP-IDF doesn't ship.
  CircuitPython builds with the component manager off, so for cameraif the build
  fetches `esp_cam_sensor`, `esp_sccb_intf` and `cmake_utilities` at their
  `deps.lock` versions into `deps/` and passes them, and `esp_h264` the same way
  for h264enc. The DEV-KIT's board sdkconfig enables the OV5647.
- **mpy-cross is built first, on its own**, for the same reason as on
  MicroPython ([the `BUILD=` trap](index.md#traps-the-build-already-handles)).
- **`BUILD=` is a short link.** make is given `BUILD=build-<board>`, a link in
  the port directory to our build directory: raspberrypi's link step echoes
  every object path in one shell argument, and with a long absolute path in
  front of each it passes Linux's 128 KB limit on one argument.

## What a module sees

The banner is CircuitPython's, unchanged: `Adafruit CircuitPython <version> on
<date>; <board name> with <chip>`. A module that has to behave differently here
can test `CIRCUITPY`, which every CircuitPython port sets, unix included.
pygraphics also passes its own define from its `micropython.mk`, so it knows it
came in as a user C module and not through CircuitPython's shared-bindings.

Two things a module's C has to absorb on CircuitPython's MCU ports. They append
`-Werror` warnings (`sign-compare`, `float-equal`, `cast-align` on atmel-samd;
`float-equal`, `double-promotion`, `cast-align` on espressif) after the module's
flags, so a `-Wno-` meant for them goes on each object, where it lands last. And
atmel-samd has no C heap, so `malloc()` doesn't link there; use `m_malloc()`.
