#!/usr/bin/env python3
"""Prepare a MicroPython checkout for our builds.

The checkout is moved to the tag in UPSTREAM, then gets every patch in
patches/micropython/ and every module's own MicroPython patches (the *.patch
files directly inside modules/<name>/patches/, today cameraif's),
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

patches/micropython-lib/ holds fixes micropython-lib has made since the
commit MicroPython's lib/micropython-lib submodule points at. They go onto
the submodule's working tree after every update, not into the local commit,
and a patch the submodule already has (applied before, or upstream's own
after a pin move) is skipped. Every package a firmware freezes and every
copy staged from the submodule for a board's /lib then carries them.
"""

import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OVERLAY = REPO / "patches" / "micropython"
LIB_OVERLAY = REPO / "patches" / "micropython-lib"
LIB = Path("lib") / "micropython-lib"
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


def lib_series():
    """The micropython-lib patches, in order."""
    return sorted(LIB_OVERLAY.glob("*.patch"))


def _quiet(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode


def lib_applied(lib, patch):
    """True when micropython-lib's tree at `lib` already has the patch."""
    return _quiet(lib, "apply", "--reverse", "--check", str(patch)) == 0


def lib_files(patches):
    """The files the micropython-lib patches touch."""
    files = set()
    for patch in patches:
        out = subprocess.run(["git", "apply", "--numstat", str(patch)], check=True, text=True, capture_output=True).stdout
        files.update(line.split("\t", 2)[2] for line in out.splitlines() if line.count("\t") >= 2)
    return sorted(files)


def apply_lib(lib, patches):
    for patch in patches:
        name = patch.relative_to(REPO)
        if lib_applied(lib, patch):
            print(f"already in micropython-lib: {name}", flush=True)
        elif git(lib, "apply", str(patch), check=False) != 0:
            sys.exit(f"apply_patches: {name} does not apply to {LIB}")
        else:
            print(f"applied {name}", flush=True)


def lib_record(mp):
    """micropython-lib's commit and the patches of ours its tree carries, for a build's record."""
    lib = Path(mp) / LIB
    commit = git(lib, "rev-parse", "--short=12", "HEAD", capture=True)
    return {"commit": commit, "patches": [p.name for p in lib_series() if lib_applied(lib, p)]}


def series_id(patches):
    # Covers the modules' patches too, which tools/prepare-micropython.sh's id
    # left out: a changed module patch now makes a prepared tree stale.
    digest = hashlib.sha256()
    for patch in patches:
        digest.update(patch.read_bytes())
    return "Overlay-Series: " + digest.hexdigest()[:16]


def apply(mp, patches):
    for patch in patches:
        if git(mp, "apply", str(patch), check=False) != 0:
            sys.exit(f"apply_patches: {patch.relative_to(REPO)} does not apply")
        print(f"applied {patch.relative_to(REPO)}", flush=True)


def check(mp, upstream, patches):
    # A throwaway worktree at the tag, so the series is tried cumulatively on
    # a clean tree and the checkout itself (its build dirs, its submodules) is
    # never touched. A worktree rather than a clone: CI's checkout is shallow.
    with tempfile.TemporaryDirectory() as scratch:
        tree = Path(scratch) / "micropython"
        git(mp, "worktree", "add", "--quiet", "--detach", str(tree), upstream)
        try:
            apply(tree, patches)
            if lib_series():
                git(tree, "submodule", "update", "--init", "--depth", "1", "--quiet", str(LIB))
                apply_lib(tree / LIB, lib_series())
        finally:
            git(mp, "worktree", "remove", "--force", str(tree), check=False)
    print(f"the series applies clean to {upstream}", flush=True)


def prepare(mp, upstream, patches, refresh=False):
    """Apply the series once. With refresh, a tree carrying an older overlay
    is moved back to the tag and prepared again instead of refused."""
    mark = f"The PyDevices overlay applied to {upstream} (a local record, never pushed)"
    sid = series_id(patches)
    if git(mp, "log", "-1", "--format=%s", capture=True) == mark and refresh and sid not in git(
        mp, "log", "-1", "--format=%b", capture=True
    ).splitlines():
        print(f"{mp} carries an older overlay; moving it back to {upstream} and preparing again", flush=True)
        git(mp, "checkout", "--quiet", "--", "ports/esp32/lockfiles", check=False)
        git(mp, "checkout", "--quiet", upstream)
    if git(mp, "log", "-1", "--format=%s", capture=True) == mark:
        if sid not in git(mp, "log", "-1", "--format=%b", capture=True).splitlines():
            sys.exit(
                f"{mp} carries an older overlay than this series. Go back to the tag "
                f"and prepare again:\n  git -C {mp} checkout {upstream} && {sys.argv[0]} {mp}"
            )
        head = git(mp, "log", "-1", "--format=%h", capture=True)
        print(f"overlay already applied: {head} on {upstream}", flush=True)
    else:
        # A submodule with edits inside it doesn't count (micropython-lib carries
        # ours, taken off and put back below; a port's build can touch its own).
        # --porcelain spells that " M", like a submodule moved to another
        # commit, so it is git that leaves them out, not a match on the letter.
        dirty = git(
            mp, "status", "--porcelain", "--untracked-files=no", "--ignore-submodules=dirty", capture=True
        ).splitlines()
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
        print(f"overlay applied: {head} on {upstream}", flush=True)
    # Every manifest require()s from micropython-lib, so a clone without it
    # cannot even list its C modules. A port's other submodules come from
    # `make submodules`, the upstream way. Our micropython-lib patches come
    # off first, so an update to another commit isn't refused over them, and
    # go back on after.
    lib = Path(mp) / LIB
    lib_patches = lib_series()
    if lib_patches and (lib / ".git").exists():
        git(lib, "checkout", "--quiet", "--", *lib_files(lib_patches), check=False)
    git(mp, "submodule", "update", "--init", "--quiet", str(LIB))
    apply_lib(lib, lib_patches)


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
