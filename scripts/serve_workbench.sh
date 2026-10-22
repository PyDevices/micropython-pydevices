#!/bin/sh
# Open your webassembly build in workbench, the browser IDE whose simulator
# runs MicroPython.
#
#   ./build_mp.py --port webassembly --variant pydevices --modules all
#   scripts/serve_workbench.sh [build dir]
#
# It downloads the newest workbench release (once per release, into
# deps/workbench/), puts your build's micropython.mjs and micropython.wasm in
# place of the ones it ships, and serves the result on port 8000 until you press
# Ctrl+C. In the webassembly dev container that port is forwarded and your
# browser opens it; elsewhere, open http://localhost:8000/.
#
# The build dir defaults to builds/webassembly/pydevices. The page and your
# build share one origin, so nothing else needs setting up.
#
# Environment:
#   WORKBENCH_PORT   the port to serve on (default 8000)
#   WORKBENCH_BIND   the address to listen on (default 127.0.0.1)

set -eu

say() { echo "$*"; }
fail() { echo "serve_workbench.sh: $*" >&2; exit 1; }

repo=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$repo/builds/webassembly/pydevices}
port=${WORKBENCH_PORT:-8000}
bind=${WORKBENCH_BIND:-127.0.0.1}
cache=$repo/deps/workbench
site=$repo/builds/workbench

[ -f "$build/micropython.mjs" ] && [ -f "$build/micropython.wasm" ] ||
    fail "no webassembly build in $build; build one with ./build_mp.py --port webassembly --variant pydevices --modules all"

# The newest release's tag, or the newest one already downloaded when GitHub
# can't be reached.
tag=$(curl -fsSL --retry 2 https://api.github.com/repos/PyDevices/workbench/releases/latest 2>/dev/null |
      python3 -c 'import json, sys; print(json.load(sys.stdin)["tag_name"])' 2>/dev/null) || tag=
if [ -z "$tag" ]; then
    zip=$(ls -t "$cache"/workbench-v*.zip 2>/dev/null | head -n 1) ||
        zip=
    [ -n "$zip" ] || fail "can't reach GitHub for workbench's newest release, and none is downloaded yet"
    say "GitHub can't be reached; using $(basename "$zip")"
else
    zip=$cache/workbench-$tag.zip
    if [ ! -f "$zip" ]; then
        mkdir -p "$cache"
        say "Downloading workbench $tag"
        curl -fsSL --retry 3 -o "$zip.part" \
            "https://github.com/PyDevices/workbench/releases/download/$tag/workbench-$tag.zip" ||
            fail "could not download workbench-$tag.zip"
        mv "$zip.part" "$zip"
    fi
fi

rm -rf "$site"
mkdir -p "$site"
python3 -m zipfile -e "$zip" "$site"
[ -f "$site/index.html" ] && [ -d "$site/assets/pydevices" ] ||
    fail "$(basename "$zip") has no index.html and assets/pydevices/; is it a workbench release?"

# Your build in place of the release's. The release's provenance describes its
# own runtime, so it goes; your build's record goes beside the runtime instead.
cp "$build/micropython.mjs" "$build/micropython.wasm" "$site/assets/pydevices/"
rm -f "$site/assets/pydevices/micropython.wasm.provenance"
[ ! -f "$build/pydevices-build.json" ] || cp "$build/pydevices-build.json" "$site/assets/pydevices/"

if [ -n "${CODESPACE_NAME:-}" ] && [ -n "${GITHUB_CODESPACES_PORT_FORWARDING_DOMAIN:-}" ]; then
    url="https://$CODESPACE_NAME-$port.$GITHUB_CODESPACES_PORT_FORWARDING_DOMAIN/"
else
    url="http://localhost:$port/"
fi
say "workbench $(basename "$zip" .zip | sed 's/^workbench-//') with $build"
say "Serving it at $url (Ctrl+C stops it)"
exec python3 -m http.server "$port" --bind "$bind" --directory "$site"
