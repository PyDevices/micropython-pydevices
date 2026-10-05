#!/usr/bin/env python3
"""Prepare a MicroPython checkout for our builds.

The checkout is moved to the tag in UPSTREAM, then gets every patch in
patches/micropython/ and every module's own MicroPython patches (the *.patch
files directly inside modules/<name>/patches/, today usbif's and cameraif's),
all applied once and recorded as one local commit. That commit's body names
the series it was made from, so a tree prepared before a patch changed is
caught instead of silently built.

    patches/apply_patches.py [MICROPYTHON_DIR]
    patches/apply_patches.py [MICROPYTHON_DIR] --check

MICROPYTHON_DIR defaults to micropython/ in this repository. --check applies
the same series to a throwaway copy of the tag and leaves the checkout alone;
CI runs it with no modules present, so it checks the overlay alone.

Every patch applies to the whole tree, whichever port you build: a prepared
tree serves every port and every module set.
"""

import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OVERLAY = REPO / "patches" / "micropython"
MODULES = REPO / "modules"


def git(mp, *args, check=True, capture=False):
    result = subprocess.run(
        ["git", "-C", str(mp), *args],
        check=check,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    return result.stdout.strip() if capture else result.returncode


def series():
    """The overlay's patches in order, then each module's, modules by name."""
    patches = sorted(OVERLAY.glob("*.patch"))
    if MODULES.is_dir():
        for module in sorted(MODULES.iterdir()):
            patches += sorted((module / "patches").glob("*.patch"))
    return patches


def series_id(patches):
    # Covers the modules' patches too, which tools/prepare-micropython.sh's id
    # left out: a changed usbif patch now makes a prepared tree stale.
    digest = hashlib.sha256()
    for patch in patches:
        digest.update(patch.read_bytes())
    return "Overlay-Series: " + digest.hexdigest()[:16]


def apply(mp, patches):
    for patch in patches:
        if git(mp, "apply", str(patch), check=False) != 0:
            sys.exit(f"apply_patches: {patch.relative_to(REPO)} does not apply")
        print(f"applied {patch.relative_to(REPO)}")


def check(mp, upstream, patches):
    # A throwaway worktree at the tag, so the series is tried cumulatively on
    # a clean tree and the checkout itself (its build dirs, its submodules) is
    # never touched. A worktree rather than a clone: CI's checkout is shallow.
    with tempfile.TemporaryDirectory() as scratch:
        tree = Path(scratch) / "micropython"
        git(mp, "worktree", "add", "--quiet", "--detach", str(tree), upstream)
        try:
            apply(tree, patches)
        finally:
            git(mp, "worktree", "remove", "--force", str(tree), check=False)
    print(f"the series applies clean to {upstream}")


def prepare(mp, upstream, patches):
    mark = f"The PyDevices overlay applied to {upstream} (a local record, never pushed)"
    sid = series_id(patches)
    if git(mp, "log", "-1", "--format=%s", capture=True) == mark:
        if sid not in git(mp, "log", "-1", "--format=%b", capture=True).splitlines():
            sys.exit(
                f"{mp} carries an older overlay than this series. Go back to the tag "
                f"and prepare again:\n  git -C {mp} checkout {upstream} && {sys.argv[0]} {mp}"
            )
        head = git(mp, "log", "-1", "--format=%h", capture=True)
        print(f"overlay already applied: {head} on {upstream}")
    else:
        dirty = [
            line
            for line in git(mp, "status", "--porcelain", "--untracked-files=no", capture=True).splitlines()
            if not line.startswith(" m")
        ]
        if dirty:
            sys.exit(f"{mp} has uncommitted changes; commit or discard them first")
        if git(mp, "describe", "--tags", "--exact-match", check=False, capture=True) != upstream:
            git(mp, "checkout", "--quiet", upstream)
        apply(mp, patches)
        git(mp, "add", "-A", "--", ".", ":!ports/*/build*")
        git(
            mp, "-c", "user.name=pydevices", "-c", "user.email=pydevices@local",
            "commit", "--quiet", "-m", mark, "-m", sid,
        )
        head = git(mp, "log", "-1", "--format=%h", capture=True)
        print(f"overlay applied: {head} on {upstream}")
    # Every manifest require()s from micropython-lib, so a clone without it
    # cannot even list its C modules. A port's other submodules come from
    # `make submodules`, the upstream way.
    git(mp, "submodule", "update", "--init", "--quiet", "lib/micropython-lib")


def main(argv):
    args = [a for a in argv if a != "--check"]
    mp = Path(args[0]) if args else REPO / "micropython"
    if not (mp / ".git").exists():
        sys.exit(f"not a git checkout: {mp}")
    upstream = (REPO / "UPSTREAM").read_text().strip()
    patches = series()
    if "--check" in argv:
        check(mp, upstream, patches)
    else:
        prepare(mp, upstream, patches)


if __name__ == "__main__":
    main(sys.argv[1:])
