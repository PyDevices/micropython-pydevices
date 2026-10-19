#!/usr/bin/env python3
"""Keep provenance.json in step with the tree.

provenance.json records the MicroPython pin and the SHA-256 of every patch in
patches/micropython/ and patches/tinyusb/. This script rewrites those from
the files, so nobody types a hash:

    python3 scripts/update_provenance.py            # rewrite provenance.json
    python3 scripts/update_provenance.py --check    # exit 1 if it is stale (CI)

The pin comes from UPSTREAM, which holds a full commit. The file also carries
a human label for it ("v1.30.0-preview-160"), which no file in this tree
knows. The label is kept while the pin is unchanged; when UPSTREAM moves, say
what the new commit is called:

    python3 scripts/update_provenance.py --describe v1.30.0-preview-171

The "migrated-from" note is history and is left alone.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SERIES = (("patches", "patches/micropython"), ("tinyusb_patches", "patches/tinyusb"))
UPSTREAM_RE = re.compile(r"^micropython/micropython main at (?P<sha>[0-9a-f]{12})(?: \((?P<label>.*)\))?$")


def patch_hashes(directory):
    """name -> {"sha256": ...} for every *.patch directly in the directory, in name order."""
    return {p.name: {"sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
            for p in sorted(directory.glob("*.patch"))}


def upstream_line(pin, old, describe):
    """The "upstream" string for the pin in UPSTREAM. Raises SystemExit when the
    pin has moved and no label was given."""
    short = pin[:12]
    label = describe
    if label is None:
        m = UPSTREAM_RE.match(old or "")
        if m and m["sha"] == short:
            label = m["label"]
        elif m or old:
            sys.exit("UPSTREAM is at %s but provenance.json names %r.\n"
                     "Say what the new commit is called: --describe v1.30.0-preview-NNN"
                     % (short, old))
    return "micropython/micropython main at %s%s" % (short, " (%s)" % label if label else "")


def build(repo, current, describe=None):
    """The provenance.json content the tree calls for, keeping keys it doesn't own."""
    new = dict(current)
    new["upstream"] = upstream_line((repo / "UPSTREAM").read_text().strip(), current.get("upstream"), describe)
    for key, rel in SERIES:
        new[key] = patch_hashes(repo / rel)
    return new


def render(data):
    return json.dumps(data, indent=1) + "\n"


def diff(old, new):
    """One line per difference, for the failure message."""
    out = []
    if old.get("upstream") != new.get("upstream"):
        out.append("upstream: %r -> %r" % (old.get("upstream"), new.get("upstream")))
    for key, _ in SERIES:
        a, b = old.get(key, {}), new.get(key, {})
        for n in sorted(set(a) | set(b)):
            if n not in a:
                out.append("%s: %s is not recorded" % (key, n))
            elif n not in b:
                out.append("%s: %s is recorded but the file is gone" % (key, n))
            elif a[n] != b[n]:
                out.append("%s: %s has changed" % (key, n))
    return out


def main(argv=None, repo=REPO):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="change nothing; exit 1 if provenance.json is stale")
    ap.add_argument("--describe", help="the name of the UPSTREAM commit, for when the pin has moved")
    args = ap.parse_args(argv)
    path = repo / "provenance.json"
    current = json.loads(path.read_text())
    try:
        new = build(repo, current, args.describe)
    except SystemExit as e:
        if not args.check:
            raise
        print("provenance.json is stale:\n  %s\nRun: python3 scripts/update_provenance.py --describe <name>" % str(e).split("\n")[0])
        return 1
    stale = diff(current, new)
    if args.check:
        if stale or path.read_text() != render(new):
            print("provenance.json is stale:")
            for line in stale or ["formatting differs"]:
                print("  " + line)
            print("Run: python3 scripts/update_provenance.py")
            return 1
        print("provenance.json is current (%d patches)" % sum(len(new[k]) for k, _ in SERIES))
        return 0
    path.write_text(render(new))
    print("provenance.json: %s" % ("%d change(s)" % len(stale) if stale else "already current"))
    for line in stale:
        print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
