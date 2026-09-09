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
# RC is what the caller reads. `git apply ... && echo` is exempt from `set -e`
# (a failing left-hand side of && does not exit the shell), so this script used
# to print nothing for a patch that would not apply and still exit 0 - it
# reported "3 of 5 applied" as success during the v1.29.0 survey, 2026-09-09.
RC=0
while read -r n; do
    [[ -z "$n" ]] && continue
    P=$(ls "$HERE"/patches/${n}-*.patch)
    if [[ "$MODE" == "--check" ]]; then
        # NOTE: --check tests each patch against the tree as it is now, not
        # against the tree with the earlier patches of the series applied. For
        # a cumulative series that can report a false failure; --apply on a
        # scratch worktree is the honest test.
        if git -C "$MP" apply --check "$P"; then
            echo "applies clean: $(basename "$P")"
        else
            echo "DOES NOT APPLY: $(basename "$P")" >&2; RC=1
        fi
    else
        if git -C "$MP" apply "$P"; then
            echo "applied: $(basename "$P")"
        else
            echo "FAILED TO APPLY: $(basename "$P")" >&2; RC=1
        fi
    fi
done < "$SERIES"
exit "$RC"
