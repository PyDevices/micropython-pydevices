#!/usr/bin/env python3
"""Write micropython.wasm.provenance for a webassembly build.

The website and workbench keep this file beside the runtime they serve, so a
reader can tell what a published runtime was built from: the binary's size and
SHA-256, the patch series MicroPython carried and the commit of every source
that went in. The commits come from the build's own pydevices-build.json.

    python3 scripts/wasm_provenance.py builds/webassembly/pydevices
"""

import argparse
import datetime
import hashlib
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def git(*args):
    out = subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=True)
    return out.stdout.strip() if out.returncode == 0 else None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("build", help="the build directory, holding micropython.wasm and pydevices-build.json")
    parser.add_argument("--built-by", default="", help="who built it, such as a CI run's URL")
    args = parser.parse_args()

    build = Path(args.build)
    wasm = build / "micropython.wasm"
    record = json.loads((build / "pydevices-build.json").read_text(encoding="utf-8"))
    if record.get("port") != "webassembly":
        sys.exit(f"{build} is a {record.get('port')} build, not webassembly")

    sources = {}
    for name, rev in sorted(record.get("module_revisions", {}).items()):
        if not rev or not rev.get("commit"):
            continue
        # A module named by path (LVGL's port) is named for its repository.
        if name.startswith("/"):
            name = "omnipython-lvgl" if "omnipython-lvgl" in name else Path(name).name
        sources[name] = {
            "head": rev["commit"],
            "describe": rev.get("revision") or rev["commit"][:7],
            "dirty": bool(rev.get("dirty")),
        }
    head = git("rev-parse", "HEAD")
    if head:
        sources["micropython-pydevices"] = {
            "head": head,
            "describe": git("describe", "--always", "--dirty", "--abbrev=7") or head[:7],
            "dirty": bool(git("status", "--porcelain")),
        }
    if record.get("micropython"):
        sources["micropython"] = {"describe": record["micropython"]}

    data = wasm.read_bytes()
    stamp = {
        "format": 1,
        "target": "mp-wasm",
        "port": "webassembly",
        "install_only": False,
        "built_by": args.built_by,
        "stamped": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "binary": {
            "name": wasm.name,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        },
        "modules": record.get("modules"),
        "overlays": sorted(p.name for p in (REPO / "patches" / "micropython").glob("*.patch")),
        "sources": sources,
    }
    out = build / "micropython.wasm.provenance"
    out.write_text(json.dumps(stamp, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Stamped {out} ({len(sources)} sources)")


if __name__ == "__main__":
    main()
