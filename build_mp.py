#!/usr/bin/env python3
"""Build PyDevices MicroPython firmware with one command.

    ./build_mp.py --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI \\
        --flash 16MB --modules audiodsp,displayif,/home/you/earful

Leave out --port and it walks you through port, board and variant; leave out
--board on a port that has boards and it asks for that; --variant is always
optional. Leave out --modules and it lists modules/ and asks. Our modules go
by short name, anything else by full path; "all" is every module. Arguments
it doesn't recognise go straight to make.

This script names no version and no setting. Pins live in modules.lock,
deps.lock and UPSTREAM; esp32 settings live in sdkconfig fragments found by
convention. See docs/build-plan.md.

Environment: OUT_DIR (default builds/), VARIANTS_DIR (default variants/),
MODULES_DIR (default modules/), JOBS (default: every core).
"""

import argparse
import fcntl
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent
OUT_DIR = Path(os.environ.get("OUT_DIR", REPO / "builds")).resolve()
VARIANTS_DIR = Path(os.environ.get("VARIANTS_DIR", REPO / "variants")).resolve()
MODULES_DIR = Path(os.environ.get("MODULES_DIR", REPO / "modules")).absolute()
MP = REPO / "micropython"
DEPS = REPO / "deps"
JOBS = os.environ.get("JOBS", str(os.cpu_count() or 4))

# The name a build without --variant gets for its last directory level: what
# upstream's own make calls it.
DEFAULT_VARIANT = {"unix": "standard", "windows": "standard", "webassembly": "standard"}

sys.path.insert(0, str(REPO / "patches"))
import apply_patches  # noqa: E402


def say(msg=""):
    print(msg, flush=True)


def die(msg):
    sys.exit(f"build_mp.py: {msg}")


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def git_out(path, *args):
    r = subprocess.run(["git", "-C", str(path), *args], text=True, capture_output=True)
    return r.stdout.strip() if r.returncode == 0 else ""


# ---- the workspace and what fills micropython/, modules/ and deps/ ---------

def workspace():
    """The directory our sibling checkouts live in, or None outside one.

    From a worktree (.worktrees/NAME) the siblings hang off the main checkout,
    not the worktree's parent."""
    common = git_out(REPO, "rev-parse", "--path-format=absolute", "--git-common-dir")
    main = Path(common).parent if common else REPO
    ws = main.parent
    return ws if (ws / "micropython" / ".git").exists() else None


def read_lock(name):
    rows = []
    for line in (REPO / name).read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            rows.append(line.split())
    return rows


def link(dest, target):
    dest.parent.mkdir(parents=True, exist_ok=True)
    os.symlink(os.path.relpath(target, dest.parent), dest)
    say(f"linked {dest.relative_to(REPO)} -> {target}")


def clone_at(url, ref, dest, recursive=False):
    say(f"cloning {url} at {ref} into {dest.relative_to(REPO)}")
    run(["git", "init", "-q", str(dest)])
    run(["git", "-C", str(dest), "remote", "add", "origin", url])
    run(["git", "-C", str(dest), "fetch", "-q", "--depth", "1", "origin", ref])
    run(["git", "-C", str(dest), "checkout", "-q", "FETCH_HEAD"])
    if recursive:
        run(["git", "-C", str(dest), "submodule", "update", "--init", "--recursive", "--depth", "1", "-q"])


def ensure_micropython(ws):
    if MP.exists():
        return
    if ws:
        link(MP, ws / "micropython")
    else:
        upstream = (REPO / "UPSTREAM").read_text().strip()
        say(f"cloning MicroPython {upstream}")
        run(["git", "clone", "-q", "--branch", upstream, "https://github.com/micropython/micropython", str(MP)])


def ensure_modules(ws):
    for name, url, commit in read_lock("modules.lock"):
        dest = MODULES_DIR / name
        if dest.exists() or dest.is_symlink():
            continue
        if ws and (ws / name).is_dir():
            link(dest, ws / name)
        else:
            clone_at(url, commit, dest)


def dep_version(name, path):
    """The version a toolchain checkout actually is, read the way it records it."""
    if name == "esp-idf":
        return git_out(path, "describe", "--tags", "--exact-match")
    if name == "emsdk":
        f = path / "upstream" / "emscripten" / "emscripten-version.txt"
        return f.read_text().strip().strip('"') if f.exists() else ""
    if name == "SDL2":
        h = path / "x86_64-w64-mingw32" / "include" / "SDL2" / "SDL_version.h"
        if not h.exists():
            return ""
        nums = dict(re.findall(r"#define SDL_(MAJOR_VERSION|MINOR_VERSION|PATCHLEVEL)\s+(\d+)", h.read_text()))
        return "{MAJOR_VERSION}.{MINOR_VERSION}.{PATCHLEVEL}".format(**nums)
    return ""


def ensure_dep(name, ws):
    """Fetch (or link) one toolchain and refuse if it isn't the locked version."""
    lock = {row[0]: row[1:] for row in read_lock("deps.lock")}
    url, version = lock[name]
    dest = DEPS / name
    if not (dest.exists() or dest.is_symlink()):
        if ws and (ws / name).is_dir():
            link(dest, ws / name)
        elif name == "esp-idf":
            clone_at(url, version, dest, recursive=True)
            run(["bash", "-c", f"cd {dest} && ./install.sh all"])
        elif name == "emsdk":
            run(["git", "clone", "-q", url, str(dest)])
            run(["bash", "-c", f"cd {dest} && ./emsdk install {version} && ./emsdk activate {version}"])
        elif name == "SDL2":
            DEPS.mkdir(exist_ok=True)
            with tempfile.TemporaryDirectory() as tmp:
                archive = Path(tmp) / "sdl2.tar.gz"
                say(f"downloading {url}")
                urllib.request.urlretrieve(url, archive)
                with tarfile.open(archive) as t:
                    t.extractall(tmp)
                top = next(p for p in Path(tmp).iterdir() if p.is_dir())
                shutil.move(str(top), str(dest))
    have = dep_version(name, dest.resolve())
    if have != version:
        die(f"deps/{name} is {have or 'unreadable'}, but deps.lock wants {version}")
    return dest.resolve()


# ---- what you asked for ----------------------------------------------------

def choose(prompt, options, default=None, allow_none=False):
    if not sys.stdin.isatty():
        die(f"{prompt}: not given, and there is no terminal to ask on")
    say(prompt)
    shown = (["(none)"] if allow_none else []) + options
    for i, opt in enumerate(shown, 1):
        mark = "  (default)" if opt == default or (default is None and allow_none and i == 1) else ""
        say(f"  {i:2}. {opt}{mark}")
    answer = input("> ").strip()
    if not answer:
        pick = default if default is not None else ("(none)" if allow_none else None)
    elif answer.isdigit() and 1 <= int(answer) <= len(shown):
        pick = shown[int(answer) - 1]
    elif answer in shown:
        pick = answer
    else:
        die(f"not one of the choices: {answer}")
    if pick is None:
        die("nothing chosen")
    return None if pick == "(none)" else pick


def ports():
    return sorted(p.name for p in (MP / "ports").iterdir() if (p / "Makefile").exists())


def is_board_port(port):
    return (MP / "ports" / port / "boards").is_dir()


def boards(port):
    names = {p.name for p in (MP / "ports" / port / "boards").iterdir() if (p / "board.json").exists()}
    names |= {p.name for p in (REPO / "boards" / port).glob("*") if p.is_dir()}
    return sorted(names)


def board_dir(port, board):
    """Ours while boards/ lasts (it retires in step 5 of the plan), else upstream's."""
    ours = REPO / "boards" / port / board
    return ours if ours.is_dir() else MP / "ports" / port / "boards" / board


def variants(port, board):
    if board:
        found = set()
        for f in board_dir(port, board).glob("mpconfigvariant_*.*"):
            found.add(f.stem[len("mpconfigvariant_"):])
        return sorted(found)
    found = {p.name for p in (MP / "ports" / port / "variants").iterdir() if p.is_dir()}
    found |= {p.name for p in (VARIANTS_DIR / port).glob("*") if p.is_dir()}
    return sorted(found)


def our_variant_dir(port, variant):
    d = VARIANTS_DIR / port / variant
    return d if variant and d.is_dir() else None


def module_choices():
    return sorted(p.name for p in MODULES_DIR.iterdir() if p.is_dir() and not p.name.startswith("."))


def resolve_modules(spec):
    """The comma list as modules/manifest.py will read it (paths made
    absolute), and the module directories it amounts to ("all" expanded),
    which the esp32 conventions and the build record need."""
    names, dirs = [], []
    for item in (s.strip() for s in spec.split(",")):
        if not item:
            continue
        if "/" in item or os.sep in item:
            path = Path(item).expanduser().resolve()
            if not path.is_dir():
                die(f"module path does not exist: {item}")
            names.append(str(path))
            dirs.append(path)
        elif item == "all":
            names.append("all")
            dirs += [MODULES_DIR / n for n in module_choices() if n != "all"]
        else:
            if not (MODULES_DIR / item).is_dir():
                die(f"no module '{item}' in {MODULES_DIR} (have: {', '.join(module_choices())})")
            names.append(item)
            dirs.append(MODULES_DIR / item)
    unique = []
    for d in dirs:
        if d.resolve() not in [u.resolve() for u in unique]:
            unique.append(d)
    return ",".join(names), unique


def record(dirs):
    rows = {}
    for d in dirs:
        real = d.resolve()
        rev = git_out(real, "describe", "--always", "--dirty", "--abbrev=12") or "(not a git checkout)"
        # The commit and whether anything was uncommitted: what a consumer
        # needs to tell later whether the code that went in has moved.
        commit = git_out(real, "rev-parse", "HEAD") or None
        dirty = bool(git_out(real, "status", "--porcelain", "--untracked-files=no")) if commit else None
        rows[d.name] = {"path": str(real), "revision": rev, "commit": commit, "dirty": dirty}
    return rows


# ---- esp32: the generated board directory, the fragments, autosize --------

def esp32_chip(base):
    try:
        return json.loads((base / "board.json").read_text()).get("mcu", "")
    except (OSError, ValueError):
        return ""


FLASH_SIZES = ("2MB", "4MB", "8MB", "16MB", "32MB", "64MB", "128MB")


def esp32_fragment(chip, module_dirs, board, variant, flash, table):
    """Our sdkconfig, appended last. Each source is used only if it exists."""
    parts = ["# Generated by build_mp.py. kconfgen takes the last assignment, so this", "# file is appended after the board's own (cmods#29).", ""]
    if flash:
        parts += [f"# --flash {flash}", f'CONFIG_ESPTOOLPY_FLASHSIZE="{flash}"', f"CONFIG_ESPTOOLPY_FLASHSIZE_{flash}=y", ""]
    if table:
        parts += [
            "# the partition table, grown to fit the app (autosize)",
            "CONFIG_PARTITION_TABLE_CUSTOM=y",
            f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="{table}"',
            f'CONFIG_PARTITION_TABLE_FILENAME="{table}"',
            "",
        ]
    sources = [VARIANTS_DIR / "esp32" / "sdkconfig", VARIANTS_DIR / "esp32" / f"sdkconfig.{chip}"]
    for d in module_dirs:
        sources += [d / "sdkconfig", d / f"sdkconfig.{chip}"]
    if variant:
        sources += [VARIANTS_DIR / "esp32" / board / variant / "sdkconfig"]
    used = []
    for src in sources:
        if src.is_file():
            used.append(src)
            parts += [f"# from {src}", src.read_text().rstrip(), ""]
    return "\n".join(parts) + "\n", used


def esp32_board_dir(gen, base, board, variant, fragment_path, module_dirs):
    """Upstream's board, unchanged, plus our fragment, as BOARD_DIR= (the way
    cmods' build_mp.sh did it). Everything else the board holds is linked
    through, because ${MICROPY_BOARD_DIR} is an include directory and board
    cmake files name files in it."""
    if gen.exists():
        shutil.rmtree(gen)
    gen.mkdir(parents=True)
    vfile = f"mpconfigvariant_{variant}.cmake" if variant else "mpconfigvariant.cmake"
    generated = {"mpconfigboard.cmake", "mpconfigboard.h", vfile}
    for entry in sorted(base.iterdir()):
        if entry.name not in generated:
            (gen / entry.name).symlink_to(entry.resolve())
    lines = ["# Generated by build_mp.py: the board below, unchanged.", f"include({base.as_posix()}/mpconfigboard.cmake)"]
    for d in module_dirs:
        comps = d / "components"
        for c in sorted(comps.iterdir()) if comps.is_dir() else []:
            if (c / "CMakeLists.txt").exists():
                lines.append(f"list(APPEND EXTRA_COMPONENT_DIRS {c.resolve().as_posix()})")
    (gen / "mpconfigboard.cmake").write_text("\n".join(lines) + "\n")
    if (base / "mpconfigboard.h").exists():
        (gen / "mpconfigboard.h").write_text(f'// Generated by build_mp.py: the board\'s own header.\n#include "{(base / "mpconfigboard.h").as_posix()}"\n')
    vlines = ["# Generated by build_mp.py. Our fragment comes LAST: kconfgen takes the", "# last assignment in SDKCONFIG_DEFAULTS (cmods#29)."]
    base_variant = base / vfile
    if variant and not base_variant.exists():
        die(f"{board} has no variant {variant}")
    vlines.append(f"include({base_variant.as_posix()}{'' if variant else ' OPTIONAL'})")
    vlines.append(f"list(APPEND SDKCONFIG_DEFAULTS {fragment_path.as_posix()})")
    (gen / vfile).write_text("\n".join(vlines) + "\n")


def esp32_autosize(log_text, build, port_dir, dest):
    """The table this build used, with its app partition grown to fit."""
    m = re.search(r"app partition is too small for binary \S+ size (0x[0-9a-fA-F]+)", log_text)
    if not m:
        return None
    sdk = (build / "sdkconfig").read_text()
    used = re.search(r'^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="(.*)"$', sdk, re.M)
    if not used:
        return None
    src = Path(used.group(1))
    src = src if src.is_absolute() else port_dir / src
    rows = []
    for raw in src.read_text().splitlines():
        line = raw.strip()
        if line and not line.startswith("#"):
            fields = [f.strip() for f in line.split(",")]
            rows.append((fields + [""] * 6)[:6])
    part = re.search(r"Part '([^']+)'", log_text)
    name = part.group(1) if part else "factory"
    idx = next((i for i, r in enumerate(rows) if r[0] == name), None)
    if idx is None:
        idx = next((i for i, r in enumerate(rows) if r[1] == "app"), None)
    if idx is None:
        return None

    def num(v):
        v = v.strip().lower()
        mult = 1024 if v.endswith("k") else 1024 * 1024 if v.endswith("m") else 1
        return int(v.rstrip("km"), 0) * mult

    align, headroom = 0x10000, 0x40000
    image = int(m.group(1), 16)
    size = (image + align - 1) & ~(align - 1)
    size = (size + headroom + align - 1) & ~(align - 1)
    rows[idx][4] = hex(size)
    cursor = num(rows[idx][3]) + size
    for r in rows[idx + 1:]:
        r[3] = hex(cursor)
        cursor += num(r[4])
    body = ["# Name, Type, SubType, Offset, Size, Flags",
            f"# Generated by build_mp.py from {src}: '{rows[idx][0]}' grown to {hex(size)} for an",
            f"# image of {hex(image)}. Every partition after it moved, and so does the filesystem.", ""]
    body += [", ".join(r).rstrip(", ") for r in rows]
    dest.write_text("\n".join(body) + "\n")
    return dest


# ---- the build -------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], allow_abbrev=False)
    ap.add_argument("--port")
    ap.add_argument("--board")
    ap.add_argument("--variant")
    ap.add_argument("--modules", help='comma list: short names, full paths, or "all"')
    ap.add_argument("--flash", help="esp32 flash size, e.g. 16MB")
    ap.add_argument("--no-autosize", action="store_true", help="esp32: refuse instead of growing the app partition")
    ap.add_argument("--clean", action="store_true", help="delete this target's build dir first")
    args, make_extra = ap.parse_known_args()

    ws = workspace()
    ensure_micropython(ws)
    ensure_modules(ws)
    mp = MP.resolve()
    # One build at a time in a MicroPython checkout: preparing rewrites the
    # tree, and two esp32 builds race on the port's managed_components/. The
    # lock sits beside the checkout, so in the workspace it is the one the
    # other build tools there take (the anchor's .micropython-build.lock).
    # Held until this process exits.
    lock = open(mp.parent / ".micropython-build.lock", "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        say(f"waiting for another build to release {lock.name}")
        fcntl.flock(lock, fcntl.LOCK_EX)
    upstream = (REPO / "UPSTREAM").read_text().strip()
    apply_patches.prepare(mp, upstream, apply_patches.series(), refresh=True)

    port = args.port or choose("Port:", ports())
    if port not in ports():
        die(f"no port '{port}' (have: {', '.join(ports())})")
    board = args.board
    if is_board_port(port):
        board = board or choose("Board:", boards(port))
        if board not in boards(port):
            die(f"no board '{board}' for {port}")
    elif board:
        die(f"{port} has no boards; leave out --board")
    variant = args.variant
    if variant is None and not args.port:
        variant = choose("Variant:", variants(port, board), allow_none=True)
    if variant and variant not in variants(port, board):
        die(f"no variant '{variant}' (have: {', '.join(variants(port, board)) or 'none'})")
    if args.flash and port != "esp32":
        die("--flash is for esp32")
    if args.flash and args.flash.upper() not in FLASH_SIZES:
        die(f"--flash takes one of {', '.join(FLASH_SIZES)}")
    flash = args.flash.upper() if args.flash else None

    spec = args.modules
    if spec is None:
        if not sys.stdin.isatty():
            die("--modules: not given, and there is no terminal to ask on")
        say("Modules: " + ", ".join(module_choices()))
        spec = input('Which (comma list; full paths for others; "all"; empty for none)? ').strip()
    spec, module_dirs = resolve_modules(spec)

    build = OUT_DIR / port / (board or "") / (variant or DEFAULT_VARIANT.get(port, "default"))
    build = Path(os.path.normpath(build))
    rec_path = build / "pydevices-build.json"
    want = {"port": port, "board": board, "variant": variant, "flash": flash, "modules": spec}
    if args.clean and build.exists():
        shutil.rmtree(build)
        say(f"--clean: removed {build}")
    if rec_path.exists():
        had = json.loads(rec_path.read_text())
        if {k: had.get(k) for k in want} != want:
            shutil.rmtree(build)
            say(f"{build} was built with {had.get('modules')!r} (flash {had.get('flash')}); wiped, building {spec!r}")
    elif build.exists() and any(build.iterdir()):
        # Something built here without a record: nobody can say with what.
        shutil.rmtree(build)
        say(f"{build} has no build record; wiped")
    build.mkdir(parents=True, exist_ok=True)
    # Written before the build, so a build that fails still says what it was
    # built with, and the next one with a different set wipes it.
    rec_path.write_text(json.dumps(dict(want, complete=False), indent=1) + "\n")

    env = dict(os.environ)
    for leak in ("USER_C_MODULES", "FROZEN_MANIFEST", "BUILD", "BOARD", "VARIANT", "BOARD_DIR", "VARIANT_DIR"):
        env.pop(leak, None)
    env["PYDEVICES_MODULES"] = spec
    # A prebuilt mpy-cross, named in the environment: neither make nor CMake
    # then runs the mpy-cross sub-make that would inherit BUILD= (micropython#19667).
    run(["make", "-C", str(mp / "mpy-cross"), "-j", JOBS], env={k: v for k, v in env.items() if k != "PYDEVICES_MODULES"},
        stdout=subprocess.DEVNULL)
    env["MICROPY_MPYCROSS"] = str(mp / "mpy-cross" / "build" / "mpy-cross")

    port_dir = mp / "ports" / port
    make = ["make", "-C", str(port_dir), "-j", JOBS, f"BUILD={build}"]
    prefix = ""
    ours = our_variant_dir(port, variant) if not board else None
    manifest = ours / "manifest.py" if ours and (ours / "manifest.py").exists() else MODULES_DIR / "manifest.py"
    make.append(f"FROZEN_MANIFEST={manifest}")
    if board:
        make.append(f"BOARD={board}")
        if variant:
            make.append(f"BOARD_VARIANT={variant}")
    elif ours:
        make.append(f"VARIANT_DIR={ours}")
    elif variant:
        make.append(f"VARIANT={variant}")

    if port == "esp32":
        idf = ensure_dep("esp-idf", ws)
        prefix = f'. "{idf}/export.sh" >/dev/null && '
    elif port == "webassembly":
        emsdk = ensure_dep("emsdk", ws)
        prefix = f'. "{emsdk}/emsdk_env.sh" >/dev/null 2>&1 && '
    elif port == "windows":
        make.append("CROSS_COMPILE=x86_64-w64-mingw32-")
        if any(d.resolve().name == "displayif" for d in module_dirs):
            make.append(f"SDL2_DEV={ensure_dep('SDL2', ws)}")
    make += make_extra

    def shell(cmd, log=None):
        line = prefix + shlex.join(cmd)
        if log is None:
            return subprocess.run(["bash", "-c", line], env=env).returncode
        p = subprocess.Popen(["bash", "-c", line], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        out = []
        for chunk in p.stdout:
            sys.stdout.write(chunk)
            out.append(chunk)
        return p.wait(), "".join(out)

    rc = 0
    try:
        if port == "esp32":
            base = board_dir(port, board)
            chip = esp32_chip(base)
            gen = build / board
            frag = build / "sdkconfig.pydevices"
            table = None
            for attempt in (1, 2):
                text, used = esp32_fragment(chip, module_dirs, board, variant, flash, table)
                frag.write_text(text)
                esp32_board_dir(gen, base, board, variant, frag, module_dirs)
                make_esp = [c for c in make if not c.startswith("BOARD=")] + [f"BOARD={board}", f"BOARD_DIR={gen}"]
                # A saved sdkconfig beats SDKCONFIG_DEFAULTS; ours is generated (cmods#29).
                (build / "sdkconfig").unlink(missing_ok=True)
                if attempt == 1:
                    say(f"esp32 {board}{' ' + variant if variant else ''} ({chip}); our sdkconfig: {frag}")
                    for u in used:
                        say(f"  fragment: {u}")
                    shell(make_esp + ["submodules"])
                rc, out = shell(make_esp, log=True)
                if rc == 0 or attempt == 2:
                    break
                grown = esp32_autosize(out, build, port_dir, build / "partitions.csv")
                if not grown:
                    break
                if args.no_autosize:
                    say(f"\nThe app doesn't fit its partition. A table that fits: {grown}\n(--no-autosize, so nothing was rebuilt.)")
                    break
                say(f"\nautosize: the app didn't fit; building once more against {grown}.")
                say("  Growing the app partition moves the filesystem: a board flashed with this")
                say("  image comes up with an empty filesystem (cmods#30).\n")
                table = grown
            if rc == 0 and table:
                sdk = (build / "sdkconfig").read_text()
                got = re.search(r'^CONFIG_PARTITION_TABLE_FILENAME="(.*)"$', sdk, re.M)
                if not got or Path(got.group(1)) != table:
                    die(f"the build used {got.group(1) if got else 'no'} partition table, not {table} (cmods#29)")
                say("partition layout in this image:\n" + "\n".join("  " + l for l in table.read_text().splitlines() if l and not l.startswith("#")))
        else:
            shell(make + ["submodules"])
            rc = shell(make)
    finally:
        if port == "esp32":
            # The component manager rewrites these on every build; the tree's own copy is the record.
            subprocess.run(["git", "-C", str(mp), "checkout", "--quiet", "--", "ports/esp32/lockfiles"], check=False)

    if rc != 0:
        die(f"the build failed (make exit {rc})")
    rec = dict(want, complete=True)
    rec["micropython"] = git_out(mp, "describe", "--always", "--abbrev=12")
    rec["module_revisions"] = record(module_dirs)
    rec_path.write_text(json.dumps(rec, indent=1) + "\n")
    say(f"\nBuilt into {build}")
    for name in ("firmware.bin", "firmware.uf2", "micropython", "micropython.exe", "micropython.mjs", "micropython.wasm"):
        if (build / name).exists():
            say(f"  {build / name}")


if __name__ == "__main__":
    main()
