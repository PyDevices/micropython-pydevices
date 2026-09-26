#!/usr/bin/env bash
# Build the CAST_P4 spike image in the shared micropython checkout, under the
# workspace build lock; restore the generated lockfiles afterwards.
set -uo pipefail
WS=/home/brad/gh/pydevices
HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/fetch_esp_h264.sh"
exec 9>"$WS/.micropython-build.lock"; flock 9
source "$WS/esp-idf/export.sh" >/dev/null
cd "$WS/micropython/ports/esp32"
git -C "$WS/micropython" status --short > "$HERE/build.status.before"
make BOARD_DIR="$HERE/boards/CAST_P4" BOARD_VARIANT=PRE_REV3_C6_WIFI -j"${JOBS:-6}" "$@"
rc=$?
git -C "$WS/micropython" status --short > "$HERE/build.status.after"
for f in $(git -C "$WS/micropython" status --short | awk '/dependencies.lock/ {print $2}'); do
  git -C "$WS/micropython" checkout -- "$f"
done
echo "build exit $rc; checkout status after restore:"; git -C "$WS/micropython" status --short
exit $rc
