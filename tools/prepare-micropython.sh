#!/usr/bin/env bash
# Prepare a MicroPython checkout for the workspace's builds: the pinned tag,
# this repository's whole patch series and the module-tied patches applied
# once and recorded as a local commit, and micropython-lib initialised so a
# manifest can name its C modules.
#
# The layout is the one the presets assume: this repository, the module
# repositories and the MicroPython checkout are siblings. Then, from
# <micropython>/ports/<port>:
#   make VARIANT_DIR=../../../micropython-pydevices/variants/unix/pydevices
#   make BOARD_DIR=../../../micropython-pydevices/boards/esp32/LILYGO_T_EMBED_S3 BOARD_VARIANT=SPIRAM_OCT
# with FROZEN_MANIFEST=../../../micropython-pydevices/manifests/<preset>.py to
# pick what is aboard. See manifests/README.md.
#
# Usage: tools/prepare-micropython.sh [MICROPYTHON_DIR]
#   MICROPYTHON_DIR  an existing clone of micropython/micropython (default:
#                    `micropython` beside this repository). It is checked out
#                    at the pinned tag if it is not there already; a tree with
#                    the overlay commit already on top is left alone, unless
#                    patches/ has changed since; then it says how to redo it.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# The siblings hang off the main checkout, also when this runs from one of
# its worktrees (micropython-pydevices/.worktrees/NAME), whose parent is not
# where the module repositories live.
COMMON=$(git -C "$HERE" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)
if [[ -n "$COMMON" ]]; then
    SRC=$(cd "$(dirname "$COMMON")/.." && pwd)
else
    SRC=$(cd "$HERE/.." && pwd)
fi
MP=${1:-$SRC/micropython}
UPSTREAM=$(tr -d '[:space:]' < "$HERE/UPSTREAM")
MARK="The PyDevices overlay applied to $UPSTREAM (a local record, never pushed)"
# Which series the overlay commit was made from, so a tree prepared before a
# patch was added or changed is caught instead of silently built.
SERIES_ID="Overlay-Series: $(cat "$HERE"/patches/*.patch | sha256sum | cut -c1-16)"

[[ -d "$MP/.git" ]] || { echo "not a git checkout: $MP (clone micropython/micropython there first)" >&2; exit 1; }

if [[ "$(git -C "$MP" log -1 --format=%s 2>/dev/null)" == "$MARK" ]]; then
    if ! git -C "$MP" log -1 --format=%b | grep -qxF "$SERIES_ID"; then
        echo "$MP carries an older overlay than patches/ holds. Go back to the tag and prepare again:" >&2
        echo "  git -C $MP checkout $UPSTREAM && $0 $MP" >&2
        exit 1
    fi
    echo "overlay already applied: $(git -C "$MP" log -1 --format=%h) on $UPSTREAM"
else
    if [[ -n "$(git -C "$MP" status --porcelain --untracked-files=no | grep -v '^ m')" ]]; then
        echo "$MP has uncommitted changes; commit or discard them first" >&2; exit 1
    fi
    if [[ "$(git -C "$MP" describe --tags --exact-match 2>/dev/null)" != "$UPSTREAM" ]]; then
        git -C "$MP" checkout --quiet "$UPSTREAM"
    fi
    for profile in windows-full webassembly-pydevices esp32-s3-debug esp32-audio esp32-webrepl esp32-tinyusb; do
        "$HERE/apply.sh" "$profile" "$MP"
    done
    # Module-tied patches, from the repositories that need them (retool draft,
    # "Patches"): each script defaults to a `micropython` beside its repo, so
    # the directory is passed explicitly here.
    # A module that is missing is an error, not a skip: an overlay without
    # its patches builds, and builds the wrong thing.
    for mod in usbif cameraif; do
        if [[ ! -x "$SRC/$mod/apply_patches.sh" ]]; then
            echo "missing $SRC/$mod/apply_patches.sh: clone $mod beside micropython-pydevices, then prepare again from the tag" >&2
            exit 1
        fi
        "$SRC/$mod/apply_patches.sh" --apply "$MP"
    done
    git -C "$MP" add -A -- . ':!ports/*/build*'
    git -C "$MP" -c user.name=pydevices -c user.email=pydevices@local commit --quiet -m "$MARK" -m "$SERIES_ID"
    echo "overlay applied: $(git -C "$MP" log -1 --format=%h) on $UPSTREAM"
fi
# Every manifest require()s from micropython-lib, so a clone without it cannot
# even list its C modules. The rest of a port's submodules come from
# `make -C ports/<port> submodules`, the upstream way, once per port.
git -C "$MP" submodule update --init --quiet lib/micropython-lib
echo "micropython: $MP"
