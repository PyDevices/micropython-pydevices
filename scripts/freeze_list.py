#!/usr/bin/env python3
"""List what a manifest freezes, by name and hash, and compare two lists.

    scripts/freeze_list.py list --port-dir micropython/ports/unix \\
        --env PYDEVICES_MODULES=all variants/unix/pydevices/manifest.py > after.json
    scripts/freeze_list.py compare before.json after.json

`list` evaluates a manifest the way MicroPython's tools/makemanifest.py does
(the same variables, the same manifestfile.py), without compiling anything,
and writes one JSON document: every file the build would freeze, under the
name it is imported as, with the sha256 of its source and of the text that is
frozen (a package with a version in its metadata gets a `__version__` line
appended, as mip installs it), and the version of every package a require()
brought in. Give it the manifest, port directory and environment a build uses.

`compare` says what is in one list and not the other, what has the same name
and different content, and what differs only in the appended `__version__`
line. It exits 1 when anything differs beyond that line.
"""

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def frozen_text(path, metadata):
    """What tools/makemanifest.py freezes for a .py file: the source, universal
    newlines, with `__version__` appended unless the file sets one."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if metadata.version and "__version__ =" not in text:
        text += "\n\n__version__ = {}\n".format(repr(metadata.version))
    return text


def evaluate(mpy_dir, port_dir, board_dir, mpy_lib_dir, env, manifest):
    sys.path.insert(0, str(Path(mpy_dir) / "tools"))
    import manifestfile

    for k, v in env.items():
        os.environ[k] = v
    variables = {"MPY_DIR": mpy_dir, "PORT_DIR": port_dir, "BOARD_DIR": board_dir,
                 "MPY_LIB_DIR": mpy_lib_dir or str(Path(mpy_dir) / "lib" / "micropython-lib")}
    variables = {k: os.path.abspath(v) for k, v in variables.items() if v}

    # Which require()d package each metadata belongs to, and its version.
    owner = {}
    stack = []
    real_include = manifestfile.ManifestFile.include
    real_update = manifestfile.ManifestPackageMetadata.update

    def include(self, manifest_path, is_require=False, **kwargs):
        if is_require and isinstance(manifest_path, str):
            where = self._resolve_path(manifest_path)
            stack.append(os.path.basename(where.rstrip("/\\")) if os.path.isdir(where)
                         else os.path.basename(os.path.dirname(where)))
        try:
            return real_include(self, manifest_path, is_require=is_require, **kwargs)
        finally:
            if is_require and isinstance(manifest_path, str):
                stack.pop()

    def update(self, mode, **kwargs):
        owner[id(self)] = (stack[-1] if stack else None, kwargs.get("version"))
        return real_update(self, mode, **kwargs)

    manifestfile.ManifestFile.include = include
    manifestfile.ManifestPackageMetadata.update = update
    try:
        m = manifestfile.ManifestFile(manifestfile.MODE_FREEZE, variables)
        m.execute(manifest)
        results = list(m.files())
    except manifestfile.ManifestFileError as e:
        sys.exit(f"freeze_list.py: manifest error: {e.args[0]}")
    finally:
        manifestfile.ManifestFile.include = real_include
        manifestfile.ManifestPackageMetadata.update = real_update

    files, packages = {}, {}
    for r in results:
        name, version = owner.get(id(r.metadata), (None, None))
        if r.full_path.endswith(".py"):
            text = frozen_text(r.full_path, r.metadata)
            frozen = sha256_bytes(text.encode("utf-8"))
        else:
            frozen = sha256_bytes(Path(r.full_path).read_bytes())
        # makemanifest hashes nothing; a source file's own bytes are the plainest identity.
        source = sha256_bytes(Path(r.full_path).read_bytes())
        if r.target_path in files:
            print(f"freeze_list.py: {r.target_path} is frozen twice "
                  f"({files[r.target_path]['from']} and {r.full_path})", file=sys.stderr)
        files[r.target_path] = {"source_sha256": source, "frozen_sha256": frozen, "opt": r.opt,
                                "package": name, "version": r.metadata.version, "from": r.full_path}
        if name:
            packages[name] = version
    return {"files": dict(sorted(files.items())), "packages": dict(sorted(packages.items())),
            "c_modules": sorted(m.c_modules())}


def cmd_list(args):
    env = dict(e.split("=", 1) for e in args.env or [])
    mpy_dir = args.mpy_dir or str(REPO / "micropython")
    port_dir = args.port_dir
    doc = evaluate(os.path.realpath(mpy_dir), port_dir, args.board_dir, args.mpy_lib_dir, env, args.manifest)
    if args.strip_paths:
        for f in doc["files"].values():
            f.pop("from", None)
    text = json.dumps(doc, indent=1) + "\n"
    if args.output:
        Path(args.output).write_text(text)
    else:
        sys.stdout.write(text)
    print(f"{len(doc['files'])} files, {len(doc['packages'])} required packages", file=sys.stderr)
    return 0


def cmd_compare(args):
    a = json.loads(Path(args.before).read_text())
    b = json.loads(Path(args.after).read_text())
    fa, fb = a["files"], b["files"]
    only_a = sorted(set(fa) - set(fb))
    only_b = sorted(set(fb) - set(fa))
    same = changed = version_line = 0
    differ, opt_differs = [], []
    for name in sorted(set(fa) & set(fb)):
        x, y = fa[name], fb[name]
        if x["opt"] != y["opt"]:
            opt_differs.append(name)
        if x["frozen_sha256"] == y["frozen_sha256"]:
            same += 1
        elif x["source_sha256"] == y["source_sha256"]:
            # Same source file, different appended __version__ line.
            version_line += 1
        else:
            differ.append(name)
    print(f"before: {len(fa)} files, after: {len(fb)} files")
    print(f"identical frozen content:                        {same}")
    print(f"same source file, different appended __version__: {version_line}")
    print(f"same name, different source:                     {len(differ)}")
    print(f"compiled at a different -O level:                {len(opt_differs)}")
    print(f"only before:                                     {len(only_a)}")
    print(f"only after:                                      {len(only_b)}")
    for label, names in (("DIFFERENT", differ), ("OPT", opt_differs), ("ONLY BEFORE", only_a), ("ONLY AFTER", only_b)):
        for n in names:
            print(f"  {label}: {n}")
    pa, pb = a["packages"], b["packages"]
    print("required packages (name version):")
    for n in sorted(set(pa) | set(pb)):
        mark = "" if pa.get(n, 0) == pb.get(n, 0) and n in pa and n in pb else "   <- differs"
        print(f"  {n:<28} before {pa.get(n, '-')!s:<12} after {pb.get(n, '-')!s:<12}{mark}")
    if args.version_lines:
        for n in sorted(set(fa) & set(fb)):
            x, y = fa[n], fb[n]
            if x["frozen_sha256"] != y["frozen_sha256"] and x["source_sha256"] == y["source_sha256"]:
                print(f"  version line: {n}: {x['version']} -> {y['version']}")
    return 1 if (differ or opt_differs or only_a or only_b) else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], epilog=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    ls = sub.add_parser("list", help="evaluate a manifest and write what it freezes")
    ls.add_argument("manifest")
    ls.add_argument("--mpy-dir", help="the MicroPython checkout (default: micropython/ beside build_mp.py)")
    ls.add_argument("--port-dir", required=True, help="$(PORT_DIR): the port's directory")
    ls.add_argument("--board-dir", help="$(BOARD_DIR)")
    ls.add_argument("--mpy-lib-dir", help="$(MPY_LIB_DIR) (default: <mpy-dir>/lib/micropython-lib)")
    ls.add_argument("--env", action="append", metavar="NAME=VALUE", help="environment the manifest reads (PYDEVICES_MODULES, ...)")
    ls.add_argument("--output", "-o", help="write here instead of standard output")
    ls.add_argument("--strip-paths", action="store_true", help="leave each file's path out of the list")
    ls.set_defaults(func=cmd_list)
    cp = sub.add_parser("compare", help="compare two lists by name and hash")
    cp.add_argument("before")
    cp.add_argument("after")
    cp.add_argument("--version-lines", action="store_true", help="name every file that differs only in its appended __version__")
    cp.set_defaults(func=cmd_compare)
    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
