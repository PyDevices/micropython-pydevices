#!/usr/bin/env python3
"""Say what a change can reach in the workflows that build firmware.

A pull request is diffed against its base; a push to main from the commit main
was at before the push. A manual or scheduled run, a base git can't find, and
a file no rule names reach everything.

    ci_scope.py circuitpython --event pull_request --base <sha>
        the jobs of circuitpython.yml: unix, unix_modules, usbhost
    ci_scope.py firmware --event pull_request --base <sha>
        release-firmware.yml: build (anything to build), full (the whole set,
        so the assemble job can check it) and only (the cells to build when it
        is not the whole set)

    ci_scope.py circuitpython --files modules/pngio/src/pngio.c

Prints the answer and, under GitHub Actions, writes it to $GITHUB_OUTPUT.
"""

import argparse
import ast
import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# A glob over the path from the repo root: `*` stays in one directory, `**`
# crosses them.


def glob(pattern):
    parts = re.split(r"(\*\*|\*)", pattern)
    return re.compile("".join(".*" if p == "**" else "[^/]*" if p == "*" else re.escape(p) for p in parts) + r"\Z")


def matches(path, patterns):
    return any(glob(p).match(path) for p in patterns)


# Files that no workflow here reads: docs, the release page's stager, the
# other kinds of board and every unit test file (the gate job runs those).
INERT = (
    "**.md", "docs/**", "NOTICE", "LICENSE", "lib-overrides/**", "boards/**", "variants/**",
    "provenance.json", "scripts/stage_firmware_page.py", "scripts/pin_lvgl_ref.sh",
    "scripts/update_provenance.py", "scripts/freeze_list.py", "scripts/serve_workbench.sh",
    "scripts/exercise_build.sh", "tests/**", ".gitignore", ".github/dependabot.yml",
)

# What the three CircuitPython jobs can read. They build CircuitPython, so
# MicroPython's patches, its pin and TinyUSB's patches reach none of them, and
# no job builds a module's examples or docs.
CP_COMMON = ("build_mp.py", "deps.lock", "modules.lock", "patches/circuitpython/**",
             "modules/library.py", "modules/manifest.py", "scripts/ci_scope.py")
CP_NOT_BUILT = ("UPSTREAM", "patches/micropython/**", "patches/micropython-lib/**", "patches/tinyusb/**",
                "patches/apply_patches.py", "modules/*/examples/**", "modules/*/docs/**")
# The modules the unix-modules job names. The rest (audioif, picotts, tflite,
# vision, audiocomponents, all) are not in its list.
UNIX_MODULES = ("displayif", "pydevices", "pdwidgets", "palettes", "pngio", "tsmux", "ppa", "jpegio", "cameraif",
                "mdns", "usbhost", "castif", "h264enc")
CP_JOBS = {
    # pygraphics comes from modules.lock: nothing under modules/ is built with it.
    "unix": CP_COMMON,
    "unix_modules": CP_COMMON + tuple("modules/%s/**" % m for m in UNIX_MODULES),
    # The ESP32-P4 board cell with usbhost: its sources, not its tests or tools.
    "usbhost": CP_COMMON + ("modules/usbhost/**",),
}
CP_EXCLUDE = {"usbhost": ("modules/usbhost/tests/**", "modules/usbhost/tools/**")}
CP_WORKFLOW = ".github/workflows/circuitpython.yml"


def cp_reach(path):
    if path == CP_WORKFLOW:
        return set(CP_JOBS)
    if matches(path, INERT) or matches(path, CP_NOT_BUILT) or path.startswith(".github/"):
        return set()      # another workflow runs on its own events
    if path.startswith("modules/") and path.count("/") > 1:
        # A file inside a module's directory reaches the jobs that build that module.
        return {j for j, pats in CP_JOBS.items()
                if matches(path, pats) and not matches(path, CP_EXCLUDE.get(j, ()))}
    jobs = {j for j, pats in CP_JOBS.items() if matches(path, pats)}
    return jobs or set(CP_JOBS)   # a path no rule names


# ---- release firmware ------------------------------------------------------

FIRMWARE = "scripts/release_firmware.py"
FIRMWARE_WORKFLOW = ".github/workflows/release-firmware.yml"
# The pins every build fetches: a change to one can break any board.
FIRMWARE_PINS = ("modules.lock", "deps.lock")


def cells_of(source):
    """(cells by id, the rest of the module without CELLS) from release_firmware.py."""
    tree = ast.parse(source)
    cells = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, "id", "") == "CELLS" for t in node.targets):
            for call in node.value.elts:
                row = {k.arg: ast.literal_eval(k.value) for k in call.keywords}
                cells[row["id"]] = row
    rest = ast.Module([n for n in tree.body if not (isinstance(n, ast.Assign)
                       and any(getattr(t, "id", "") == "CELLS" for t in n.targets))], [])
    return cells, ast.dump(rest)


def firmware_scope(files, show_base, show_head):
    """(build, full, only): anything to build, the whole set, or just these cells.

    The workflow, or anything in release_firmware.py besides CELLS, builds the
    whole set. A change to CELLS builds the cells whose row changed, and the
    whole set when a cell is added or removed. A pin in modules.lock or
    deps.lock builds the whole set too. Everything else the workflow watches is
    read by the unit tests its gate job runs and by no build."""
    if FIRMWARE_WORKFLOW in files or any(p in files for p in FIRMWARE_PINS):
        return True, True, []
    if FIRMWARE not in files:
        return False, False, []
    old, old_rest = cells_of(show_base())
    new, new_rest = cells_of(show_head())
    if old_rest != new_rest or set(old) != set(new):
        return True, True, []
    changed = sorted(i for i in new if old[i] != new[i] and not new[i].get("optional"))
    return bool(changed), False, changed


# ---- git -------------------------------------------------------------------

def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def changed_files(base, head):
    if not base or re.fullmatch(r"0+", base):
        return None
    try:
        git("cat-file", "-e", base + "^{commit}")
    except subprocess.CalledProcessError:
        try:
            git("fetch", "--no-tags", "--depth=1", "origin", base)
        except subprocess.CalledProcessError:
            return None
    try:
        return git("diff", "--name-only", "--no-renames", base, head).split("\n")[:-1]
    except subprocess.CalledProcessError:
        return None


def emit(values):
    for k, v in values.items():
        print("%s=%s" % (k, v))
    out = os.environ.get("GITHUB_OUTPUT")
    if out:
        with open(out, "a") as f:
            for k, v in values.items():
                f.write("%s=%s\n" % (k, v))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("workflow", choices=("circuitpython", "firmware"))
    ap.add_argument("--event", default="")
    ap.add_argument("--base", default="")
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--files", nargs="*")
    args = ap.parse_args(argv)

    if args.files is not None:
        files = args.files
    elif args.event in ("pull_request", "push"):
        files = changed_files(args.base, args.head)
    else:
        files = None
    if files is None:
        print("Everything runs (event %r, base %r)." % (args.event, args.base))
    else:
        print("Scope: %d changed files." % len(files))

    if args.workflow == "circuitpython":
        jobs = set(CP_JOBS) if files is None else set().union(*(cp_reach(f) for f in files)) if files else set()
        if files:
            for f in files:
                print("  %-60s %s" % (f, ", ".join(sorted(cp_reach(f))) or "-"))
        emit({j: "true" if j in jobs else "false" for j in CP_JOBS})
    else:
        if files is None:
            emit({"build": "true", "full": "true", "only": ""})
        else:
            build, full, only = firmware_scope(
                files,
                lambda: git("show", "%s:%s" % (args.base, FIRMWARE)),
                lambda: (REPO / FIRMWARE).read_text() if args.head == "HEAD" else git("show", "%s:%s" % (args.head, FIRMWARE)))
            emit({"build": str(build).lower(), "full": str(full).lower(), "only": ",".join(only)})
    return 0


if __name__ == "__main__":
    sys.exit(main())
