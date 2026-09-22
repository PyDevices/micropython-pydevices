#!/usr/bin/env bash
# Lay out a work directory in which MicroPython builds with upstream's own
# commands and nothing of ours but this repository:
#
#   work/
#   ├── micropython/            a clone of the pinned tag, this repo's profile applied
#   ├── micropython-pydevices/  this repository (a symlink)
#   ├── <module repos>/         symlinks: audiodsp, audioif, displayif, ...
#   ├── ulab/  mp3/             upstream dependencies audiodsp's glue looks for beside it
#   └── esp-idf/ emsdk/ SDL2/   toolchains, symlinked from where they already are
#
# Then, from work/micropython/ports/<port>:
#   make VARIANT_DIR=../../../micropython-pydevices/variants/unix/pydevices
#   make BOARD_DIR=../../../micropython-pydevices/boards/esp32/LILYGO_T_EMBED_S3 BOARD_VARIANT=SPIRAM_OCT
# with FROZEN_MANIFEST=../../../micropython-pydevices/manifests/<preset>.py to
# pick what is aboard. See manifests/README.md.
#
# Usage: tools/make-work-dir.sh [WORK_DIR] [SOURCE_ROOT]
#   WORK_DIR    where to build (default ~/gh/work)
#   SOURCE_ROOT the directory the repositories are cloned in (default: the
#               parent of this repository)
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${1:-$HOME/gh/work}
SRC=${2:-$(cd "$HERE/.." && pwd)}
CMODS="$SRC/cmods"
UPSTREAM=$(tr -d '[:space:]' < "$HERE/UPSTREAM")

mkdir -p "$WORK"
link() { # link NAME TARGET
    if [[ -e "$WORK/$1" && ! -L "$WORK/$1" ]]; then echo "error: $WORK/$1 exists and is not a symlink" >&2; exit 1; fi
    [[ -e "$2" ]] || { echo "skip $1: $2 does not exist" >&2; return 0; }
    ln -sfn "$2" "$WORK/$1"
}
for repo in micropython-pydevices audiodsp audioif displayif cameraif usbif lvgl-micropython lvgl-bindings pygraphics pdwidgets palettes ulab mpvst; do
    link "$repo" "$SRC/$repo"
done
link mp3 "$CMODS/mp3"
link esp-idf "$CMODS/esp-idf"
link emsdk "$CMODS/emsdk"
sdl=$(ls -d "$CMODS"/SDL2-[0-9]* 2>/dev/null | head -1 || true)
[[ -n "$sdl" ]] && link SDL2 "$sdl"

# The MicroPython clone: objects borrowed from the existing checkout (nothing
# is copied), checked out at the pinned tag, with this repository's whole
# patch series applied once and left applied. It is not a symlink to that
# checkout on purpose: build_mp.sh applies and reverts its own overlays on
# that tree, and two owners of one tree is how half-patched builds happen.
if [[ ! -d "$WORK/micropython/.git" ]]; then
    git clone --quiet --shared --branch "$UPSTREAM" "$CMODS/micropython" "$WORK/micropython"
    for profile in windows-full webassembly-pydevices esp32-s3-debug esp32-audio; do
        "$HERE/apply.sh" "$profile" "$WORK/micropython"
    done
    # Module-tied patches, until they have a non-patch shape (retool draft,
    # "Patches"): usbif's from its own repository, cameraif's from cmods.
    [[ -x "$SRC/usbif/apply_patches.sh" ]] && "$SRC/usbif/apply_patches.sh" --apply "$WORK/micropython"
    for p in "$CMODS"/patches/cameraif-*.patch; do
        [[ -f "$p" ]] && git -C "$WORK/micropython" apply "$p"
    done
    git -C "$WORK/micropython" -c user.name=work -c user.email=work@local commit --quiet -am "The PyDevices overlay applied to $UPSTREAM (a local record, never pushed)" || true
fi
# Every manifest require()s from micropython-lib, so a clone without it cannot
# even list its C modules. The rest of a port's submodules come from
# `make -C ports/<port> submodules`, the upstream way, once per port.
git -C "$WORK/micropython" submodule update --init --quiet lib/micropython-lib
echo "work directory: $WORK"
ls -la "$WORK" | awk '/^[ld]/{print "  "$9" "$10" "$11}'
