#!/bin/sh
# Exercise build_mp.py from a bare clone, with no boards attached.
#
#   curl -fsSL https://raw.githubusercontent.com/PyDevices/micropython-pydevices/main/scripts/exercise_build.sh | sh
#   curl -fsSL .../exercise_build.sh | sh -s -- some-branch
#   curl -fsSL .../exercise_build.sh | TARGETS="unix wasm" sh
#
# Clones micropython-pydevices into a fresh directory outside any PyDevices
# workspace, so build_mp.py has to fetch MicroPython, every module and each
# toolchain itself. It checks the failure paths, builds every desktop port and
# two esp32 boards with --modules all, two Picos with the module sets a
# display project uses, and writes a report. What should match
# on any machine: module commits, the frozen and built-in module lists, and an
# esp32 build's sdkconfig and partition layout. What won't: sizes and SHAs,
# which carry the build date and absolute paths. It needs no board, and a few
# GB of disk for the toolchains.
#
# Environment:
#   WORK       where to clone and build (default ./exercise); must not exist
#   TARGETS    space-separated subset of: unix wasm windows s3 p4 pico pico2
#              (default all)
#   DEPS_FROM  a directory holding esp-idf/, emsdk/ and SDL2/ to link instead
#              of fetching
#
#   exercise_build.sh --report DIR...   report on existing build dirs only
#
# Plain POSIX sh, and everything runs from main() on the last line, so a
# download cut short runs nothing. Nothing reads stdin: piped, stdin is this
# script.

set -u

URL=https://github.com/PyDevices/micropython-pydevices.git
LAST=
REPORT=/dev/null
LOGS=

say() { echo "$*" | tee -a "$REPORT"; }
section() { say ""; say "== $*"; }
sha() { sha256sum "$1" | cut -c1-16; }
want() { case " $TARGETS " in *" $1 "*) return 0 ;; esac; return 1; }

report_build() {  # build dir
    rb_dir=$1
    python3 - "$rb_dir" <<'EOF' | tee -a "$REPORT"
import json, sys
d = json.load(open(sys.argv[1] + "/pydevices-build.json"))
print("record    complete=%s micropython=%s modules=%s" % (d.get("complete"), d.get("micropython"), d.get("modules")))
for name, m in sorted(d.get("module_revisions", {}).items()):
    print("  module  %-18s %s dirty=%s" % (name, m.get("commit"), m.get("dirty")))
EOF
    rb_fz=$(find "$rb_dir" -name frozen_content.c -not -path "*/managed_components/*" | head -1)
    if [ -n "$rb_fz" ]; then
        awk '/const char mp_frozen_names\[\]/,/^};/' "$rb_fz" | grep -o '"[^"]*\\0"' | LC_ALL=C sort > "$rb_dir/frozen-names.txt"
        say "frozen    $(wc -l < "$rb_dir/frozen-names.txt") files, list sha $(sha "$rb_dir/frozen-names.txt")"
    fi
    rb_md=$(find "$rb_dir" -name moduledefs.h -path "*genhdr*" | head -1)
    if [ -n "$rb_md" ]; then
        grep -o 'MODULE_DEF_[A-Za-z0-9_]*' "$rb_md" | LC_ALL=C sort -u > "$rb_dir/builtin-modules.txt"
        say "builtin   $(wc -l < "$rb_dir/builtin-modules.txt") modules, list sha $(sha "$rb_dir/builtin-modules.txt")"
    fi
    if [ -f "$rb_dir/sdkconfig" ]; then
        # The partition table's path names the build dir: hash it as <build>.
        rb_abs=$(cd "$rb_dir" && pwd)
        grep '^CONFIG_' "$rb_dir/sdkconfig" | sed "s|$rb_abs|<build>|g" | LC_ALL=C sort > "$rb_dir/sdkconfig.sorted"
        say "sdkconfig $(wc -l < "$rb_dir/sdkconfig.sorted") settings, sha $(sha "$rb_dir/sdkconfig.sorted")"
        for rb_k in ESPTOOLPY_FLASHSIZE FREERTOS_HZ SPI_FLASH_AUTO_SUSPEND ESP_COREDUMP_ENABLE_TO_FLASH \
                    USB_HOST_HUBS_SUPPORTED USB_HOST_HW_BUFFER_BIAS_IN CAMERA_OV5647 BT_NIMBLE_HS_FLOW_CTRL \
                    LWIP_TCP_WND_DEFAULT SPIRAM_MALLOC_ALWAYSINTERNAL; do
            say "  $(grep -E "^CONFIG_$rb_k=" "$rb_dir/sdkconfig" || echo "CONFIG_$rb_k unset")"
        done
    fi
    if [ -f "$rb_dir/partitions.csv" ]; then
        say "partitions (autosize grew the app):"
        grep -v '^#' "$rb_dir/partitions.csv" | grep . | sed 's/^/  /' | tee -a "$REPORT"
    fi
    for rb_f in micropython micropython.exe micropython.mjs micropython.wasm micropython.bin firmware.bin firmware.uf2; do
        [ -f "$rb_dir/$rb_f" ] && say "output    $rb_f $(stat -c %s "$rb_dir/$rb_f") bytes, sha $(sha "$rb_dir/$rb_f")"
    done
    return 0
}

expect_fail() {  # name, expected text, build_mp args...
    ef_name=$1
    ef_want=$2
    shift 2
    ./build_mp.py "$@" < /dev/null > "$LOGS/fail-$ef_name.log" 2>&1
    ef_rc=$?
    if [ $ef_rc -ne 0 ] && grep -qF -- "$ef_want" "$LOGS/fail-$ef_name.log"; then
        say "PASS  $ef_name: \"$ef_want\""
    else
        say "FAIL  $ef_name: exit $ef_rc, wanted \"$ef_want\" (logs/fail-$ef_name.log)"
    fi
}

build() {  # label, build_mp args...
    b_label=$1
    shift
    section "build $b_label: build_mp.py $*"
    b_t0=$(date +%s)
    ./build_mp.py "$@" < /dev/null > "$LOGS/build-$b_label.log" 2>&1
    b_rc=$?
    say "exit $b_rc after $(( $(date +%s) - b_t0 )) s (logs/build-$b_label.log)"
    grep -E "^autosize|wiped" "$LOGS/build-$b_label.log" | tee -a "$REPORT"
    b_dir=$(grep -o 'Built into .*' "$LOGS/build-$b_label.log" | tail -1 | cut -c12-)
    if [ $b_rc -eq 0 ] && [ -n "$b_dir" ]; then
        report_build "$b_dir"
        LAST=$b_dir
    else
        say "FAILED; the last lines:"
        tail -15 "$LOGS/build-$b_label.log" | sed 's/^/  | /' | tee -a "$REPORT"
        LAST=
    fi
}

main() {
    if [ "${1:-}" = "--report" ]; then
        shift
        for m_d in "$@"; do
            section "report $m_d"
            report_build "$m_d"
        done
        return 0
    fi

    REF=${1:-main}
    WORK=${WORK:-$PWD/exercise}
    TARGETS=${TARGETS:-unix wasm windows s3 p4 pico pico2}
    # The desktop board config opens an SDL window; a headless machine has no display.
    SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy}
    export SDL_VIDEODRIVER

    if [ -e "$WORK" ]; then
        echo "$WORK exists; point WORK somewhere new" >&2
        return 2
    fi
    mkdir -p "$WORK" && WORK=$(cd "$WORK" && pwd) || return 2
    LOGS=$WORK/logs
    REPORT=$WORK/report.txt
    mkdir -p "$LOGS"
    : > "$REPORT"

    section "machine"
    say "date      $(date -u +%Y-%m-%dT%H:%MZ)"
    say "uname     $(uname -srm)"
    say "cores     $(nproc)"
    say "gcc       $(gcc --version 2>/dev/null | head -1)"
    say "python3   $(python3 --version 2>&1)"
    say "mingw     $(x86_64-w64-mingw32-gcc --version 2>/dev/null | head -1 || echo missing)"
    say "node      $(node --version 2>/dev/null || echo missing)"
    say "arm       $(arm-none-eabi-gcc --version 2>/dev/null | head -1 || echo missing)"

    section "clone"
    git clone -q --branch "$REF" "$URL" "$WORK/micropython-pydevices" < /dev/null || { say "clone failed"; return 1; }
    MPP=$WORK/micropython-pydevices
    say "micropython-pydevices $(git -C "$MPP" rev-parse HEAD) ($REF)"
    if [ -n "${DEPS_FROM:-}" ]; then
        mkdir -p "$MPP/deps"
        for m_dep in esp-idf emsdk SDL2; do
            [ -d "$DEPS_FROM/$m_dep" ] && ln -s "$DEPS_FROM/$m_dep" "$MPP/deps/$m_dep" && say "deps/$m_dep linked from $DEPS_FROM"
        done
    fi
    cd "$MPP" || return 1

    # Each must fail, and say why in words a person can act on. The first one
    # also fetches MicroPython and every module, which every later step reuses.
    section "failure paths (each must fail with the quoted text)"
    expect_fail unknown-module "no module 'nosuchmodule'" --port unix --modules nosuchmodule
    expect_fail unknown-variant "no variant 'NOPE'" --port esp32 --board ESP32_GENERIC_S3 --variant NOPE --modules ulab
    expect_fail unix-board "unix has no boards" --port unix --board ESP32_GENERIC_S3 --modules ulab
    expect_fail flash-on-unix "--flash is for esp32" --port unix --flash 16MB --modules ulab
    expect_fail no-terminal "there is no terminal to ask on" --port unix

    if want unix; then
        # A changed module set must wipe the target and say so.
        build unix-small --port unix --modules ulab
        build unix-rewipe --port unix --modules ulab,pygraphics
        if [ -n "$LAST" ] && grep -q "wiped" "$LOGS/build-unix-rewipe.log"; then
            say "PASS  a different module set wiped the target and rebuilt it"
        else
            say "FAIL  a different module set: no wipe, or no rebuild"
        fi
        build unix-all --port unix --variant pydevices --modules all
        if [ -n "$LAST" ]; then
            section "run unix-all"
            (cd "$WORK" && env -u MICROPYPATH "$LAST/micropython" -c "
import sys, ulab, pygraphics, displaydev, board_config, audioeffects, audioinstruments, palettes, pdwidgets
from ulab import numpy as np
print('RUN', sys.implementation._machine, np.sum(np.array([1, 2, 3])), board_config.display_drv.width)
" < /dev/null 2>&1 | tail -3 | tee -a "$REPORT")
        fi
    fi
    if want wasm; then
        build wasm-all --port webassembly --variant pydevices --modules all
        if [ -n "$LAST" ] && command -v node > /dev/null; then
            section "run wasm-all under node"
            printf 'import sys, ulab, pygraphics, displaydev\nprint("RUN", sys.platform, ulab.__version__)\n' > "$WORK/hello.py"
            (cd "$WORK" && timeout 60 node "$LAST/micropython.mjs" hello.py < /dev/null 2>&1 | tail -3 | tee -a "$REPORT")
        fi
    fi
    if want windows; then
        if command -v x86_64-w64-mingw32-gcc > /dev/null; then
            build windows-all --port windows --variant pydevices --modules all
        else
            section "build windows-all: SKIPPED, x86_64-w64-mingw32-gcc is missing (apt-get install gcc-mingw-w64-x86-64)"
        fi
    fi
    # esp32 without a board: the image, its sdkconfig and its partition layout.
    if want s3; then
        build s3-lcd7 --port esp32 --board ESP32_GENERIC_S3 --variant LCD_7 --flash 8MB --modules all
    fi
    if want p4; then
        build p4-devkit --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI --flash 16MB --modules all
    fi

    # rp2: what a display project on a Pico carries, not all (which doesn't fit).
    for m_t in pico pico2; do
        want $m_t || continue
        if ! command -v arm-none-eabi-gcc > /dev/null; then
            section "build $m_t: SKIPPED, arm-none-eabi-gcc is missing (apt-get install gcc-arm-none-eabi libnewlib-arm-none-eabi)"
        elif [ $m_t = pico ]; then
            build pico --port rp2 --board RPI_PICO --modules displayif,jpegio,pygraphics,palettes,pdwidgets
        else
            build pico2 --port rp2 --board RPI_PICO2 --modules displayif,jpegio,lvgl-micropython
        fi
    done

    section "done"
    say "report: $REPORT"
    say "logs:   $LOGS"
}

main "$@"
