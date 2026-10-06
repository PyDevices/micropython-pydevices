#!/bin/sh
# Put esp-vision at its pinned commit in modules/.esp-vision/src, once.
#
# In the PyDevices workspace a clone beside the anchor (../esp-vision from the
# workspace root, i.e. ~/gh/esp-vision) at the pinned commit is linked
# instead; ESP_VISION_DIR names one anywhere else. Otherwise the pinned commit
# alone is fetched, without submodules: our modules use none of them.
# Prints the directory it settled on. The directory starts with a dot, so
# build_mp.py never offers it as a module and "all" never includes it.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src="$here/src"
url=$(awk '!/^#/ && NF { print $2 }' "$here/ESP_VISION.lock")
pin=$(awk '!/^#/ && NF { print $3 }' "$here/ESP_VISION.lock")
at_pin() { [ "$(git -C "$1" rev-parse HEAD 2>/dev/null)" = "$pin" ]; }

if [ -e "$src" ] || [ -L "$src" ]; then
    at_pin "$src" || { echo "esp-vision: $src is not at $pin; delete it to refetch" >&2; exit 1; }
    echo "$src"; exit 0
fi
for cand in "${ESP_VISION_DIR:-}" "$here/../../../../esp-vision"; do
    if [ -n "$cand" ] && [ -d "$cand" ] && at_pin "$cand"; then
        ln -s "$(cd "$cand" && pwd)" "$src"
        echo "$src"; exit 0
    fi
done
git init -q "$src"
git -C "$src" remote add origin "$url"
git -C "$src" fetch -q --depth 1 origin "$pin"
git -C "$src" checkout -q FETCH_HEAD
echo "$src"
