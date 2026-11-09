# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Pick the files of audioeffects that a few named effects need.

The whole package is about 400 KB of bytecode, more than a 4 MB board has
room for beside Bluetooth or LVGL. This finds what the named effects import
(the shared base, and any effect a rack builds on) by following the import
statements, and writes an ``__init__.py`` whose ``ALL`` and lazy class table
list only them. ``from audioeffects import Reverb`` and ``create("Reverb",
...)`` work as before for the effects you chose; any other effect raises
AttributeError or ImportError.

The manifest beside this file uses it when ``AUDIOEFFECTS`` names effects.
Run it to put the same selection on a board's filesystem:

    python modules/audioeffects/selection.py chorus,reverb --out build-ae --mpy-cross mpy-cross

Name an effect by its module (``reverb``) or its class (``Reverb``).
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
DEFAULT_SOURCE = os.path.join(HERE, "..", "..", "deps", "omnipython-lib", "library", "audioeffects", "audioeffects")
PACKAGE = "audioeffects"

# Modules every selection carries: the package and the base every effect
# builds on.
ALWAYS = ("__init__", "_component")


def _init_tree(root: str) -> tuple[str, ast.Module]:
    path = os.path.join(root, "__init__.py")
    with open(path, encoding="utf-8") as f:
        text = f.read()
    return text, ast.parse(text, path)


def _assignment(tree: ast.Module, name: str) -> ast.Assign | None:
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == name for t in node.targets):
            return node
    return None


def classes(root: str) -> dict[str, str]:
    """Class name -> module, from the package's lazy class table."""
    _text, tree = _init_tree(root)
    node = _assignment(tree, "_MODULES")
    if node is None:
        raise ValueError(
            f"the audioeffects at {root} imports every effect when it loads, so a few can't be frozen alone; "
            "it needs a release that loads each effect on first use")
    return ast.literal_eval(node.value)


def resolve(root: str, names: list[str]) -> list[str]:
    """The modules for *names*, each a module or class name, in any case."""
    table = classes(root)
    modules = {m.lower(): m for m in table.values()}
    by_class = {c.lower(): m for c, m in table.items()}
    picked, unknown = [], []
    for name in names:
        module = modules.get(name.lower()) or by_class.get(name.lower())
        if module is None:
            unknown.append(name)
        elif module not in picked:
            picked.append(module)
    if unknown:
        raise ValueError(f"not effects of audioeffects: {', '.join(unknown)} (they are {', '.join(sorted(modules.values()))})")
    return picked


def _imports(path: str, table: dict[str, str]) -> set[str]:
    """Every ``audioeffects`` module *path* imports, anywhere in it,
    including an effect it reaches by class name through the package."""
    found = set()
    with open(path, encoding="utf-8") as f:
        tree = ast.parse(f.read(), path)
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom):
            if node.level == 1:
                base = node.module
            elif node.level == 0 and node.module and node.module.startswith(PACKAGE + "."):
                base = node.module[len(PACKAGE) + 1 :]
            elif node.level == 0 and node.module == PACKAGE:
                base = None
            else:
                continue
            if base:
                found.add(base)
            else:
                for alias in node.names:
                    found.add(table.get(alias.name, alias.name))
        elif isinstance(node, ast.Import):
            for alias in node.names:
                if alias.name.startswith(PACKAGE + "."):
                    found.add(alias.name[len(PACKAGE) + 1 :])
        elif isinstance(node, ast.Attribute) and node.attr in table:
            # audioeffects.Reverb, or a create("Reverb", ...) name below.
            found.add(table[node.attr])
        elif isinstance(node, ast.Constant) and isinstance(node.value, str) and node.value in table:
            found.add(table[node.value])
    return found


def files(root: str, names: list[str]) -> list[str]:
    """The files, relative to *root*, that effects *names* need."""
    table = classes(root)
    todo = list(ALWAYS[1:]) + resolve(root, names)
    seen = {"__init__": os.path.join(root, "__init__.py")}
    while todo:
        module = todo.pop()
        path = os.path.join(root, module + ".py")
        if module in seen or not os.path.isfile(path):
            continue
        seen[module] = path
        todo += _imports(path, table)
    return sorted(os.path.relpath(p, root) for p in seen.values())


def init_source(root: str, modules: list[str]) -> str:
    """The package's ``__init__.py`` with its class table and ``__all__``
    cut down to the effects in *modules*."""
    text, tree = _init_tree(root)
    table = classes(root)
    kept = {c: m for c, m in table.items() if m in modules}
    lines = text.splitlines(keepends=True)
    edits = [(_assignment(tree, "_MODULES"), "_MODULES = {\n" + "".join(f"    {c!r}: {m!r},\n" for c, m in kept.items()) + "}\n")]
    exported = _assignment(tree, "__all__")
    if exported is not None:
        names = [n for n in ast.literal_eval(exported.value) if n not in table or n in kept]
        edits.append((exported, "__all__ = " + repr(names) + "\n"))
    # From the bottom up, so earlier line numbers stay put.
    for node, new in sorted(edits, key=lambda e: e[0].lineno, reverse=True):
        lines[node.lineno - 1 : node.end_lineno] = [new]
    return "".join(lines)


def write(root: str, names: list[str], out: str) -> list[str]:
    """Copy the selection to ``<out>/audioeffects``; returns the files written."""
    modules = resolve(root, names)
    dest = os.path.join(out, PACKAGE)
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)
    picked = files(root, names)
    # An effect a rack builds on is in the selection, so it goes in the table too.
    effects = [m for m in classes(root).values() if m + ".py" in picked]
    written = []
    for rel in picked:
        target = os.path.join(dest, rel)
        if rel == "__init__.py":
            with open(target, "w", encoding="utf-8") as f:
                f.write(init_source(root, sorted(set(modules) | set(effects))))
        else:
            shutil.copyfile(os.path.join(root, rel), target)
        written.append(target)
    return written


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Copy the audioeffects files a few effects need.")
    ap.add_argument("names", help="comma list of effects, e.g. chorus,reverb")
    ap.add_argument("--out", required=True, help="folder to write audioeffects/ into")
    ap.add_argument("--source", default=DEFAULT_SOURCE, help="the audioeffects package folder")
    ap.add_argument("--mpy-cross", help="compile each file with this mpy-cross and keep only the .mpy")
    args = ap.parse_args(argv)
    root = os.path.abspath(args.source)
    if not os.path.isfile(os.path.join(root, "_component.py")):
        ap.error(f"no audioeffects package at {root}: build once with --modules audioeffects, or give --source")
    names = [n.strip() for n in args.names.split(",") if n.strip()]
    try:
        written = write(root, names, args.out)
    except ValueError as e:
        ap.error(str(e))
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
