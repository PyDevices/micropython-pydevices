# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Pick the files of audioinstruments that a few named instruments need.

The whole package is large: a firmware for a 4 MB board has no room for it
beside Bluetooth or LVGL. This finds what the named instruments import
(their voices, the rack's core and DSP, the panel) by following the import
statements, and writes a ``_names.py`` that lists only them, so a menu that
walks ``audioinstruments.ALL`` finds every instrument it lists.

The manifest beside this file uses it when ``AUDIOINSTRUMENTS`` names
instruments. Run it to put the same selection on a board's filesystem:

    python modules/audioinstruments/selection.py electric_piano,mono_synth --out build-ai --mpy-cross mpy-cross

``--source`` is the package folder (the one holding ``__init__.py``), by
default the copy a build assembled under ``deps/``. With ``--mpy-cross``,
the files come out compiled. The package has no native code, so the ``.mpy``
files suit any board.
"""

import argparse
import ast
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SOURCE = os.path.join(HERE, "..", "..", "deps", "omnipython-lib", "library", "audioinstruments", "audioinstruments")
PACKAGE = "audioinstruments"

# Modules every selection carries: the package, the panel and its view, and
# the MIDI helpers a host reaches for. The rack's own imports follow from these.
ALWAYS = ("__init__", "_names", "panel", "panel_view", "midi_cc", "midi_dispatch")


def _module_file(root: str, dotted: str) -> str | None:
    """The file under *root* for ``audioinstruments.<dotted>``, or None."""
    base = os.path.join(root, *dotted.split(".")) if dotted else root
    if os.path.isfile(base + ".py"):
        return base + ".py"
    if os.path.isfile(os.path.join(base, "__init__.py")):
        return os.path.join(base, "__init__.py")
    return None


def _dotted(root: str, path: str) -> str:
    rel = os.path.relpath(path, root)[: -len(".py")].replace(os.sep, ".")
    return rel[: -len(".__init__")] if rel.endswith("__init__") else ("" if rel == "__init__" else rel)


def _lazy_tables(root: str) -> dict[str, str]:
    """Class name -> submodule, from every package's lazy-import table.

    A package that loads its classes on first use keeps a ``_LAZY`` dict
    (name -> submodule path) or a ``_REGISTRY`` of (class, stored name,
    path); code then reaches a class by name, which no import statement shows.
    """
    found = {}
    for folder, _subs, names in os.walk(root):
        if "__init__.py" not in names:
            continue
        path = os.path.join(folder, "__init__.py")
        pkg = _dotted(root, path)
        with open(path, encoding="utf-8") as f:
            tree = ast.parse(f.read(), path)
        for node in tree.body:
            if not (isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name)):
                continue
            try:
                value = ast.literal_eval(node.value)
            except ValueError:
                continue
            pairs = []
            if node.targets[0].id == "_LAZY" and isinstance(value, dict):
                pairs = list(value.items())
            elif node.targets[0].id == "_REGISTRY" and isinstance(value, tuple):
                pairs = [(key, entry[-1]) for entry in value for key in entry[:-1]]
            for key, sub in pairs:
                found[key] = (pkg + "." if pkg else "") + sub
    return found


def _imports(root: str, path: str, lazy: dict[str, str]) -> set[str]:
    """Every ``audioinstruments`` submodule *path* imports, anywhere in it,
    including a class it reaches by name through a lazy table."""
    here = _dotted(root, path)
    is_pkg = path.endswith("__init__.py")
    pkg = here if is_pkg else here.rpartition(".")[0]
    found = set()
    with open(path, encoding="utf-8") as f:
        tree = ast.parse(f.read(), path)
    # A module-level table or export list names classes for every instrument
    # without using one; the named instruments' own modules are seeded instead.
    listing = set()
    for node in tree.body:
        if isinstance(node, ast.Assign) and isinstance(node.value, (ast.Dict, ast.Tuple, ast.List, ast.Set)):
            listing.update(id(n) for n in ast.walk(node.value))
    for node in ast.walk(tree):
        word = None
        if id(node) in listing:
            pass
        elif isinstance(node, ast.Constant) and isinstance(node.value, str):
            word = node.value
        elif isinstance(node, ast.Name):
            word = node.id
        elif isinstance(node, ast.Attribute):
            word = node.attr
        elif isinstance(node, ast.alias):
            word = node.name
        if word in lazy:
            found.add(lazy[word])
        if isinstance(node, ast.Import):
            for alias in node.names:
                if alias.name == PACKAGE or alias.name.startswith(PACKAGE + "."):
                    found.add(alias.name[len(PACKAGE) + 1 :])
        elif isinstance(node, ast.ImportFrom):
            if node.level:
                parts = pkg.split(".") if pkg else []
                if node.level > 1:
                    parts = parts[: len(parts) - (node.level - 1)]
                base = ".".join(parts + ([node.module] if node.module else []))
            elif node.module and (node.module == PACKAGE or node.module.startswith(PACKAGE + ".")):
                base = node.module[len(PACKAGE) + 1 :]
            else:
                continue
            found.add(base)
            for alias in node.names:
                found.add((base + "." if base else "") + alias.name)
        elif isinstance(node, ast.Call) and getattr(node.func, "id", None) == "__import__":
            # __import__("audioinstruments._rack." + name): the fixed part only;
            # the named instruments are seeded by the caller.
            arg = node.args[0] if node.args else None
            if isinstance(arg, ast.Constant) and isinstance(arg.value, str) and arg.value.startswith(PACKAGE + "."):
                found.add(arg.value[len(PACKAGE) + 1 :])
    return found


def _names(root: str) -> dict:
    space = {}
    with open(os.path.join(root, "_names.py"), encoding="utf-8") as f:
        exec(f.read(), space)
    return {k: v for k, v in space.items() if isinstance(v, tuple)}


def files(root: str, names: list[str]) -> list[str]:
    """The files, relative to *root*, that instruments *names* need."""
    known = _names(root)
    every = set(known.get("ALL", ())) | set(known.get("SAMPLERS", ()))
    unknown = [n for n in names if n not in every]
    if unknown:
        raise ValueError(f"not instruments of audioinstruments: {', '.join(unknown)} (they are {', '.join(sorted(every))})")
    lazy = _lazy_tables(root)
    todo = list(ALWAYS)
    for name in names:
        todo += [name, "_rack.instruments." + name, "_rack.modules." + name, "_rack." + name]
    seen = {}
    while todo:
        dotted = todo.pop()
        path = _module_file(root, dotted)
        if path is None or path in seen.values():
            continue
        seen[dotted] = path
        # A module's parent packages run first.
        parts = dotted.split(".")
        todo += [".".join(parts[:i]) for i in range(1, len(parts))] + [""]
        todo += _imports(root, path, lazy)
    return sorted(os.path.relpath(p, root) for p in seen.values())


def names_source(root: str, names: list[str]) -> str:
    """A ``_names.py`` that lists only *names*."""
    known = _names(root)
    lines = [
        "# SPDX-FileCopyrightText: 2026 Brad Barnett",
        "#",
        "# SPDX-License-Identifier: MIT",
        '"""Which instruments this build of the package has: a selection."""',
        "",
    ]
    for key, value in known.items():
        kept = tuple(n for n in value if n in names)
        lines.append(f"{key} = {kept!r}")
    return "\n".join(lines) + "\n"


def write(root: str, names: list[str], out: str) -> list[str]:
    """Copy the selection to ``<out>/audioinstruments``; returns the files written."""
    dest = os.path.join(out, PACKAGE)
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    written = []
    for rel in files(root, names):
        target = os.path.join(dest, rel)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        if rel == "_names.py":
            with open(target, "w", encoding="utf-8") as f:
                f.write(names_source(root, names))
        else:
            shutil.copyfile(os.path.join(root, rel), target)
        written.append(target)
    return written


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Copy the audioinstruments files a few instruments need.")
    ap.add_argument("names", help="comma list of instruments, e.g. electric_piano,mono_synth")
    ap.add_argument("--out", required=True, help="folder to write audioinstruments/ into")
    ap.add_argument("--source", default=DEFAULT_SOURCE, help="the audioinstruments package folder")
    ap.add_argument("--mpy-cross", help="compile each file with this mpy-cross and keep only the .mpy")
    args = ap.parse_args(argv)
    root = os.path.abspath(args.source)
    if not os.path.isfile(os.path.join(root, "_names.py")):
        ap.error(f"no audioinstruments package at {root}: build once with --modules audioinstruments, or give --source")
    names = [n.strip() for n in args.names.split(",") if n.strip()]
    written = write(root, names, args.out)
    total = 0
    for path in written:
        if args.mpy_cross:
            mpy = path[: -len(".py")] + ".mpy"
            subprocess.run([args.mpy_cross, "-O3", "-o", mpy, path], check=True)
            os.remove(path)
            path = mpy
        total += os.path.getsize(path)
    print(f"{len(written)} files, {total} bytes, in {os.path.join(args.out, PACKAGE)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
