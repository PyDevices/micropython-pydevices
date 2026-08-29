#!/usr/bin/env bash
# Apply a profile's patch series to a MicroPython checkout.
#   ./apply.sh <profile> <micropython-dir> [--check]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PROFILE="$1"; MP="$2"; MODE="${3:-apply}"
SERIES="$HERE/profiles/$PROFILE.series"
[[ -f "$SERIES" ]] || { echo "unknown profile: $PROFILE" >&2; exit 1; }
EXPECT=$(tr -d '[:space:]' < "$HERE/UPSTREAM")
HAVE=$(git -C "$MP" describe --tags 2>/dev/null || echo unknown)
[[ "$HAVE" == "$EXPECT" ]] || echo "note: tree is $HAVE, overlay is pinned to $EXPECT" >&2
while read -r n; do
    [[ -z "$n" ]] && continue
    P=$(ls "$HERE"/patches/${n}-*.patch)
    if [[ "$MODE" == "--check" ]]; then
        git -C "$MP" apply --check "$P" && echo "applies clean: $(basename "$P")"
    else
        git -C "$MP" apply "$P" && echo "applied: $(basename "$P")"
    fi
done < "$SERIES"
