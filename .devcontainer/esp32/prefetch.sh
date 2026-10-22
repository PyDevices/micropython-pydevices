#!/bin/sh
# The esp32 container fetches, when it's created, what build_mp.py would fetch
# on its first esp32 build: MicroPython, the modules, and ESP-IDF with its
# compilers (about 4 GB, into the deps/ volume). In a Codespaces prebuild that
# happens once, ahead of time, so your first build starts compiling.
set -eu
cd "$(dirname "$0")/../.."
python3 -c 'import build_mp as b; b.ensure_micropython(None); b.ensure_modules(None); b.ensure_dep("esp-idf", None)'
