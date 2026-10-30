#!/usr/bin/env bash
# Point modules.lock's omnipython-lvgl row at a branch, tag or commit, for a
# build of a ref that isn't pinned yet: release-firmware.yml runs it when its
# omnipython-lvgl-ref input is given. A branch or tag becomes the commit it
# names now, since build_mp.py fetches by full commit.
#
#   scripts/pin_lvgl_ref.sh <branch|tag|40-character commit>
set -euo pipefail
ref="${1:?usage: $0 <branch|tag|commit>}"
lock="$(dirname "$0")/../modules.lock"
url=$(awk '$1=="omnipython-lvgl"{print $2}' "$lock")
[ -n "$url" ] || { echo "modules.lock has no omnipython-lvgl row" >&2; exit 1; }
if [[ "$ref" =~ ^[0-9a-f]{40}$ ]]; then
    sha=$ref
else
    # An annotated tag lists twice; its peeled line (^{}), the commit, sorts last.
    sha=$(git ls-remote "$url" "$ref" "$ref^{}" | awk 'END{print $1}')
fi
[[ "$sha" =~ ^[0-9a-f]{40}$ ]] || { echo "no branch, tag or commit '$ref' in $url" >&2; exit 1; }
sed -i -E "s|^(omnipython-lvgl[[:space:]]+[^[:space:]]+[[:space:]]+)[0-9a-f]{40}|\1$sha|" "$lock"
grep '^omnipython-lvgl' "$lock"
