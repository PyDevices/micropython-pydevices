#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Read DEPENDENCIES.lock, the one file that pins what a build fetches.

Every line is `<name> <url> <ref>`. For a git checkout the ref is its full
40-character commit, and a line may end in `# tag <tag>` when the build also
needs the tag that commit is: to fetch it so `git describe` can name it, or to
check a toolchain's own record of its version. For a download (an archive URL)
the ref is the version the unpacked files say they are. Anything else after a
`#` is a comment.

build_mp.py, the release and provenance scripts and patches/apply_patches.py
read the file through this module. Shell steps run it:

    python3 scripts/pins.py micropython            the ref (commit or version)
    python3 scripts/pins.py circuitpython tag      or url, commit, ref
    python3 scripts/pins.py --list                 every line, parsed
    python3 scripts/pins.py --key toolchains       a short hash of a group's lines
"""

import hashlib
import re
import sys
from pathlib import Path
from typing import NamedTuple

REPO = Path(__file__).resolve().parent.parent
LOCK = "DEPENDENCIES.lock"

# What each line is for. A name in no group below is a module: build_mp.py
# fetches it into modules/<name>.
MICROPYTHON = "micropython"
# Sources a build reads that are not one module directory each.
SOURCES = ("omnipython-lvgl", "omnipython-lib", "esp-vision")
# Toolchains and components, fetched into deps/ only by the builds that use them.
TOOLCHAINS = ("esp-idf", "emsdk", "SDL2", "espressif__tinyusb", "circuitpython",
              "esp_cam_sensor", "esp_sccb_intf", "cmake_utilities", "esp_h264", "usb")

COMMIT = re.compile(r"[0-9a-f]{40}")
ARCHIVE = re.compile(r"\.(zip|tar\.gz|tgz|tar\.xz|tar\.bz2)$")


class Pin(NamedTuple):
    name: str
    url: str
    ref: str          # the full commit, or a download's version
    tag: str = ""     # the tag the commit is, where the build needs it

    @property
    def download(self):
        return bool(ARCHIVE.search(self.url))

    @property
    def group(self):
        if self.name == MICROPYTHON:
            return "micropython"
        if self.name in SOURCES:
            return "sources"
        if self.name in TOOLCHAINS:
            return "toolchains"
        return "modules"


class LockError(ValueError):
    pass


def parse(text, where=LOCK):
    """{name: Pin} in file order. Raises LockError on a line that isn't a pin."""
    pins = {}
    for n, raw in enumerate(text.splitlines(), 1):
        line, _, comment = raw.partition("#")
        fields = line.split()
        if not fields:
            continue
        if len(fields) != 3:
            raise LockError(f"{where}:{n}: want '<name> <url> <ref>', got {line.strip()!r}")
        name, url, ref = fields
        m = re.fullmatch(r"\s*tag\s+(\S+)\s*", comment)
        pin = Pin(name, url, ref, m.group(1) if m else "")
        if name in pins:
            raise LockError(f"{where}:{n}: {name} is pinned twice")
        if not pin.download and not COMMIT.fullmatch(ref):
            raise LockError(f"{where}:{n}: {name} takes a full 40-character commit, not {ref!r}")
        pins[name] = pin
    return pins


def read(repo=REPO):
    path = Path(repo) / LOCK
    return parse(path.read_text(), path.name)


def get(name, repo=REPO):
    pins = read(repo)
    if name not in pins:
        raise LockError(f"{LOCK} has no {name} line")
    return pins[name]


def group(name, repo=REPO):
    """The pins of one group, in file order: micropython, sources, modules or toolchains."""
    return [p for p in read(repo).values() if p.group == name]


def key(pins):
    """A short hash of these lines, for a cache that should change only when they do."""
    text = "".join(f"{p.name} {p.url} {p.ref} {p.tag}\n" for p in pins)
    return hashlib.sha256(text.encode()).hexdigest()[:16]


def main(argv):
    try:
        pins = read()
        if argv[:1] == ["--list"]:
            for p in pins.values():
                print(f"{p.group:11} {p.name:18} {p.ref:40} {p.tag}".rstrip())
            return 0
        if argv[:1] == ["--key"] and len(argv) == 2:
            print(key(group(argv[1])))
            return 0
        if len(argv) not in (1, 2) or (argv[1:] and argv[1] not in ("url", "ref", "commit", "tag")):
            sys.exit("usage:\n" + __doc__.split("\n\n")[3])
        p = get(argv[0])
    except (LockError, OSError) as e:
        sys.exit(f"pins.py: {e}")
    field = argv[1] if len(argv) == 2 else "ref"
    if field == "commit" and p.download:
        sys.exit(f"pins.py: {p.name} is a download, pinned by version, not a commit")
    value = {"url": p.url, "ref": p.ref, "commit": p.ref, "tag": p.tag}[field]
    if not value:
        sys.exit(f"pins.py: {p.name} has no {field}")
    print(value)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
