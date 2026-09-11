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
# --check needs a tree it may modify. Build one from $MP's HEAD and throw it
# away afterwards, however we exit.
SCRATCH=""
if [[ "$MODE" == "--check" ]]; then
    SCRATCH=$(mktemp -d)
    trap 'git -C "$MP" worktree remove --force "$SCRATCH" >/dev/null 2>&1 || rm -rf "$SCRATCH"' EXIT
    rmdir "$SCRATCH"
    if ! git -C "$MP" worktree add -q --detach "$SCRATCH" HEAD 2>/dev/null; then
        echo "apply.sh: could not create a scratch worktree from $MP" >&2
        exit 2
    fi
fi

RC=0
while read -r n; do
    [[ -z "$n" ]] && continue
    P=$(ls "$HERE"/patches/${n}-*.patch)
    if [[ "$MODE" == "--check" ]]; then
        # Cumulative, and in a scratch worktree so nothing of $MP is touched.
        #
        # This used to be `git apply --check "$MP"`, which tests every patch
        # against the tree AS IT IS -- i.e. against pristine upstream, never
        # against the tree its predecessors produced. A patch that duplicated an
        # earlier one's hunks therefore passed. That happened on 2026-09-11: a
        # regenerated 0005 carried 0004's api.js changes, this check reported
        # "applies clean" for all five patches, and the mp-wasm build then died
        # on "patch failed: ports/webassembly/api.js:147".
        #
        # $MP itself is left alone deliberately: it holds build directories
        # (untracked) and submodule pointers that a rollback would destroy.
        if git -C "$SCRATCH" apply "$P"; then
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
