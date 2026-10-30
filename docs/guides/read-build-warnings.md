# Read build warnings

A build prints a few things that look wrong and aren't.

## Every port

Every esp32 and rp2 build with `all` fails its first link on size, then autosize
builds again: [Fit firmware on a small flash](fit-firmware-on-a-small-flash.md)
shows what that looks like. That failure's last line can read
`-e See [1;31mhttps://…/Build-Troubleshooting[0m`. Upstream's Makefile prints it
with `echo -e`, and where `/bin/sh` is dash, as on Debian and Ubuntu, the `-e`
comes out as text and a log shows the colour codes raw. Nothing is broken.

## esp32

- `warning: unknown kconfig symbol 'XTAL_FREQ_AUTO'` and a run of
  `CONFIG_… was replaced with CONFIG_…` lines: ESP-IDF renamed those settings
  and still maps the old names.
- `Component lockfile contents have changed`, a CMake warning that asks you to
  mention it in any issue. The build uses its own TinyUSB and a newer ESP-IDF
  than upstream's lockfile records; [`build_mp.py`](../../build_mp.py) puts the lockfile back
  afterwards.
- cmake's `Performing Test CMAKE_HAVE_LIBC_PTHREAD - Failed` and `Could NOT find
  Threads`.
- On the P4, `Detecting C compiler ABI info - failed` and the same for CXX, each
  checked again a moment later and reported `done`.
- On the first build, `WARNING: Download failure: HTTP Error 502` from ESP-IDF's
  tool installer, which tries again by itself.

## rp2

- `No installed picotool with version 2.3.0 found - building from source`, once
  per pass.
- On the first build, `PICO_TINYUSB_PATH specified, but does not contain
  hw/bsp/rp2040; USB support will be unavailable`. It comes from the pass that
  fetches submodules, before TinyUSB is checked out; the real build says
  `TinyUSB available` and builds it.
- `PICO_BTSTACK_PATH specified but content not present` and the same for
  `PICO_LWIP_PATH`, on boards without networking or Bluetooth, which don't
  fetch those libraries.

## windows

- libffi's configure says `libtool.m4: error: problem compiling CXX test
  program`; it is probing for a C++ compiler it doesn't need, and the build goes
  on.
- Cross-built from Linux, `configure: WARNING: using cross tools not prefixed
  with host triplet`: there's no MinGW-named `file` command, so it uses the
  plain one.
- Autoconf's ``The macro `AC_TRY_COMPILE' is obsolete`` and ``The preprocessor
  macro `STDC_HEADERS' is obsolete``, and four `… is deprecated
  [-Wdeprecated-declarations]` warnings, all from libffi's own sources.
