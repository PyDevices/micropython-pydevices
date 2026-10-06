#!/usr/bin/env bash
# Exercise build_mp.py from a bare clone, with no boards attached.
#
# Clones this repository into a fresh directory outside any PyDevices
# workspace, so build_mp.py has to fetch MicroPython, every module and each
# toolchain itself. It then checks the failure paths, builds every desktop port
# and two esp32 boards with --modules all, and writes a report of what should
# match on any machine (module commits, frozen and built-in module lists, the
# esp32 sdkconfig and partition layout) beside what won't (sizes and SHAs,
# which carry the build date and absolute paths).
#
#   scripts/exercise_build.sh [REF]          REF defaults to main
#
# Environment:
#   WORK       where to clone and build (default ./exercise); must not exist
#   TARGETS    space-separated subset of: unix wasm windows s3 p4 (default all)
#   DEPS_FROM  a directory holding esp-idf/, emsdk/ and SDL2/ to link instead
#              of fetching (for a reference run on a machine that has them)
#
# The report lands in $WORK/report.txt. Nothing here needs a board.

set -u
REF=${1:-main}
WORK=${WORK:-$PWD/exercise}
TARGETS=${TARGETS:-unix wasm windows s3 p4}
URL=https://github.com/PyDevices/micropython-pydevices.git
LAST=
# The desktop board config opens an SDL window; a headless machine has no display.
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy}

if [ -e "$WORK" ]; then
    echo "$WORK exists; point WORK somewhere new" >&2
    exit 2
fi
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
LOGS=$WORK/logs
REPORT=$WORK/report.txt
mkdir -p "$LOGS"
: > "$REPORT"

say() { echo "$*" | tee -a "$REPORT"; }
section() { say ""; say "== $*"; }

# --- the machine --------------------------------------------------------------
section "machine"
say "date      $(date -u +%Y-%m-%dT%H:%MZ)"
say "uname     $(uname -srm)"
say "cores     $(nproc)"
say "gcc       $(gcc --version 2>/dev/null | head -1)"
say "python3   $(python3 --version 2>&1)"
say "mingw     $(x86_64-w64-mingw32-gcc --version 2>/dev/null | head -1 || echo missing)"
say "node      $(node --version 2>/dev/null || echo missing)"

# --- the clone ----------------------------------------------------------------
section "clone"
git clone -q --branch "$REF" "$URL" "$WORK/micropython-pydevices" || { say "clone failed"; exit 1; }
MPP=$WORK/micropython-pydevices
say "micropython-pydevices $(git -C "$MPP" rev-parse HEAD) ($REF)"
if [ -n "${DEPS_FROM:-}" ]; then
    mkdir -p "$MPP/deps"
    for d in esp-idf emsdk SDL2; do
        [ -d "$DEPS_FROM/$d" ] && ln -s "$DEPS_FROM/$d" "$MPP/deps/$d" && say "deps/$d linked from $DEPS_FROM"
    done
fi
cd "$MPP" || exit 1

# --- failure paths -------------------------------------------------------------
# Each must fail, and say why in words a person can act on. The first one also
# fetches MicroPython and every module, which every later step reuses.
section "failure paths (each must fail with the quoted text)"
expect_fail() {  # name, expected text, build_mp args...
    local name=$1 want=$2
    shift 2
    ./build_mp.py "$@" < /dev/null > "$LOGS/fail-$name.log" 2>&1
    local rc=$?
    if [ $rc -ne 0 ] && grep -qF -- "$want" "$LOGS/fail-$name.log"; then
        say "PASS  $name: \"$want\""
    else
        say "FAIL  $name: exit $rc, wanted \"$want\" (logs/fail-$name.log)"
    fi
}
expect_fail unknown-module "no module 'nosuchmodule'" --port unix --modules nosuchmodule
expect_fail unknown-variant "no variant 'NOPE'" --port esp32 --board ESP32_GENERIC_S3 --variant NOPE --modules ulab
expect_fail unix-board "unix has no boards" --port unix --board ESP32_GENERIC_S3 --modules ulab
expect_fail flash-on-unix "--flash is for esp32" --port unix --flash 16MB --modules ulab
expect_fail no-terminal "there is no terminal to ask on" --port unix

# --- reporting one build ---------------------------------------------------------
sha() { sha256sum "$1" | cut -c1-16; }
report_build() {  # dir
    local dir=$1
    python3 - "$dir" <<'EOF' | tee -a "$REPORT"
import json, sys
d = json.load(open(sys.argv[1] + "/pydevices-build.json"))
print("record    complete=%s micropython=%s modules=%s" % (d.get("complete"), d.get("micropython"), d.get("modules")))
for name, m in sorted(d.get("module_revisions", {}).items()):
    print("  module  %-18s %s dirty=%s" % (name, m.get("commit"), m.get("dirty")))
EOF
    local fz
    fz=$(find "$dir" -name frozen_content.c -not -path "*/managed_components/*" | head -1)
    if [ -n "$fz" ]; then
        awk '/const char mp_frozen_names\[\]/,/^};/' "$fz" | grep -o '"[^"]*\\0"' | sort > "$dir/frozen-names.txt"
        say "frozen    $(wc -l < "$dir/frozen-names.txt") files, list sha $(sha "$dir/frozen-names.txt")"
    fi
    local md
    md=$(find "$dir" -name moduledefs.h -path "*genhdr*" | head -1)
    if [ -n "$md" ]; then
        grep -o 'MODULE_DEF_[A-Za-z0-9_]*' "$md" | sort -u > "$dir/builtin-modules.txt"
        say "builtin   $(wc -l < "$dir/builtin-modules.txt") modules, list sha $(sha "$dir/builtin-modules.txt")"
    fi
    if [ -f "$dir/sdkconfig" ]; then
        grep '^CONFIG_' "$dir/sdkconfig" | sort > "$dir/sdkconfig.sorted"
        say "sdkconfig $(wc -l < "$dir/sdkconfig.sorted") settings, sha $(sha "$dir/sdkconfig.sorted")"
        for k in ESPTOOLPY_FLASHSIZE FREERTOS_HZ SPI_FLASH_AUTO_SUSPEND ESP_COREDUMP_ENABLE_TO_FLASH \
                 USB_HOST_HUBS_SUPPORTED USB_HOST_HW_BUFFER_BIAS_IN CAMERA_OV5647 BT_NIMBLE_HS_FLOW_CTRL \
                 LWIP_TCP_WND_DEFAULT SPIRAM_MALLOC_ALWAYSINTERNAL; do
            say "  $(grep -E "^CONFIG_$k=" "$dir/sdkconfig" || echo "CONFIG_$k unset")"
        done
    fi
    if [ -f "$dir/partitions.csv" ]; then
        say "partitions (autosize grew the app):"
        grep -v '^#' "$dir/partitions.csv" | grep . | sed 's/^/  /' | tee -a "$REPORT" > /dev/null
    fi
    for f in micropython micropython.exe micropython.mjs micropython.wasm micropython.bin firmware.bin; do
        [ -f "$dir/$f" ] && say "output    $f $(stat -c %s "$dir/$f") bytes, sha $(sha "$dir/$f")"
    done
}

build() {  # label, build_mp args...
    local label=$1
    shift
    section "build $label: build_mp.py $*"
    local t0=$SECONDS
    ./build_mp.py "$@" < /dev/null > "$LOGS/build-$label.log" 2>&1
    local rc=$?
    say "exit $rc after $((SECONDS - t0)) s (logs/build-$label.log)"
    grep -E "^autosize|wiped" "$LOGS/build-$label.log" | tee -a "$REPORT" > /dev/null
    local dir
    dir=$(grep -o 'Built into .*' "$LOGS/build-$label.log" | tail -1 | cut -c12-)
    if [ $rc -eq 0 ] && [ -n "$dir" ]; then
        report_build "$dir"
        LAST=$dir
    else
        say "FAILED; the last lines:"
        tail -15 "$LOGS/build-$label.log" | sed 's/^/  | /' | tee -a "$REPORT" > /dev/null
        LAST=
    fi
}

want() { case " $TARGETS " in *" $1 "*) return 0 ;; esac; return 1; }

# --- desktop ------------------------------------------------------------------------
if want unix; then
    # A changed module set must wipe the target and say so.
    build unix-small --port unix --modules ulab
    build unix-rewipe --port unix --modules ulab,pygraphics
    grep -q "wiped" "$LOGS/build-unix-rewipe.log" && say "PASS  a different module set wiped the target" \
        || say "FAIL  no wipe message for a different module set"
    build unix-all --port unix --variant pydevices --modules all
    if [ -n "$LAST" ]; then
        section "run unix-all"
        (cd "$WORK" && env -u MICROPYPATH "$LAST/micropython" -c "
import sys, ulab, pygraphics, displaydev, board_config, audioeffects, audioinstruments, palettes, pdwidgets
from ulab import numpy as np
print('RUN', sys.implementation._machine, np.sum(np.array([1, 2, 3])), board_config.display_drv.width)
" 2>&1 | tail -3 | tee -a "$REPORT")
    fi
fi
if want wasm; then
    build wasm-all --port webassembly --variant pydevices --modules all
    if [ -n "$LAST" ] && command -v node > /dev/null; then
        section "run wasm-all under node"
        printf 'import sys, ulab, pygraphics, displaydev\nprint("RUN", sys.platform, ulab.__version__)\n' > "$WORK/hello.py"
        (cd "$WORK" && timeout 60 node "$LAST/micropython.mjs" hello.py 2>&1 | tail -3 | tee -a "$REPORT")
    fi
fi
if want windows; then
    if command -v x86_64-w64-mingw32-gcc > /dev/null; then
        build windows-all --port windows --variant pydevices --modules all
    else
        section "build windows-all: SKIPPED, x86_64-w64-mingw32-gcc is missing (apt-get install gcc-mingw-w64-x86-64)"
    fi
fi

# --- esp32, no boards: the image, its sdkconfig and its partition layout -------------------
if want s3; then
    build s3-lcd7 --port esp32 --board ESP32_GENERIC_S3 --variant LCD_7 --flash 8MB --modules all
fi
if want p4; then
    build p4-devkit --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI --flash 16MB --modules all
fi

section "done"
say "report: $REPORT"
say "logs:   $LOGS"
