#!/usr/bin/env python3
"""Build PyDevices MicroPython firmware with one command.

    ./build_mp.py --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI \\
        --flash 16MB --modules audiodsp,displayif,/home/you/earful

Leave out --port and it walks you through port, board and variant; leave out
--board on a port that has boards and it asks for that; --variant is always
optional. Leave out --modules and it lists modules/ and asks. Our modules go
by short name, anything else by full path; "all" is every module. Arguments
it doesn't recognise go straight to make.

    ./build_mp.py --interpreter circuitpython --port raspberrypi \\
        --board adafruit_feather_rp2040 --modules pygraphics

--interpreter circuitpython builds CircuitPython-compatible firmware instead:
CircuitPython at its deps.lock pin, our C modules compiled in through its
USER_C_MODULES. Ports and boards are then CircuitPython's own.

This script names no version and no setting. Pins live in modules.lock,
deps.lock and UPSTREAM; esp32 settings live in sdkconfig fragments found by
convention. See docs/build-plan.md.

Environment: OUT_DIR (default builds/), VARIANTS_DIR (default variants/),
MODULES_DIR (default modules/), JOBS (default: every core), and on Windows
MSYS2_ROOT (default C:\\msys64), the MSYS2 whose make and MinGW gcc build the
windows port.
"""

import argparse
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

try:
    import fcntl
except ImportError:  # Windows: msvcrt's byte-range lock stands in for flock
    fcntl = None
    import msvcrt

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


# ---- native Windows: MSYS2's make, gcc and bash ---------------------------

# The shell make's recipes run under. On Windows it is resolved by
# windows_toolchain(), never left to PATH: from PowerShell or cmd, a bare
# "bash" is WSL's launcher in WindowsApps, which would run the build inside
# Linux against Windows paths.
BASH = "bash"

MSYS2_STEPS = """install MSYS2 and its MinGW toolchain, from PowerShell:
    winget install --id MSYS2.MSYS2 -e
    C:\\msys64\\usr\\bin\\bash.exe -lc "pacman -Syu --noconfirm"
    C:\\msys64\\usr\\bin\\bash.exe -lc "pacman -S --needed --noconfirm make mingw-w64-x86_64-gcc autoconf automake libtool"
then run this again. MSYS2 somewhere other than C:\\msys64? Set MSYS2_ROOT."""


def windows_toolchain():
    """On native Windows, put MSYS2's MinGW and tools first on PATH (for this
    process and every child) and pick its bash, so a plain PowerShell needs no
    setup. Returns the MSYS2 root, or None if there isn't one."""
    global BASH
    root = Path(os.environ.get("MSYS2_ROOT", r"C:\msys64"))
    if (root / "usr" / "bin" / "bash.exe").is_file():
        os.environ["PATH"] = os.pathsep.join([str(root / "mingw64" / "bin"), str(root / "usr" / "bin"), os.environ.get("PATH", "")])
        BASH = str(root / "usr" / "bin" / "bash.exe")
    else:
        root = None
        found = shutil.which("bash")
        # WindowsApps' and System32's bash.exe are WSL's.
        if found and not any(part.lower() in ("windowsapps", "system32") for part in Path(found).parts):
            BASH = found
        else:
            BASH = None
    # libffi's configure guesses its host from uname, which says MSYS (POSIX
    # emulation) unless MSYSTEM names the MinGW environment.
    os.environ.setdefault("MSYSTEM", "MINGW64")
    return root


def check_windows_tools():
    missing = [t for t in ("make", "gcc") if not shutil.which(t)] + ([] if BASH else ["bash (not WSL's)"])
    if missing:
        die(f"no {', '.join(missing)} for the windows port; {MSYS2_STEPS}")


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
    # Git for Windows defaults core.autocrlf=true: patches checked out CRLF don't apply.
    run(["git", "-C", str(dest), "config", "core.autocrlf", "false"])
    run(["git", "-C", str(dest), "remote", "add", "origin", url])
    # A tag is fetched as a tag, so the checkout can still say which one it is.
    spec = ref if re.fullmatch(r"[0-9a-f]{40}", ref) else f"refs/tags/{ref}:refs/tags/{ref}"
    run(["git", "-C", str(dest), "fetch", "-q", "--depth", "1", "origin", spec])
    run(["git", "-C", str(dest), "-c", "advice.detachedHead=false", "checkout", "-q", ref if spec != ref else "FETCH_HEAD"])
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
        run(["git", "-c", "advice.detachedHead=false", "clone", "-q", "--depth", "1", "--branch", upstream, "https://github.com/micropython/micropython", str(MP)])


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
        # ESP-IDF's own record of its version, which needs no git tags.
        f = path / "tools" / "cmake" / "version.cmake"
        nums = dict(re.findall(r"set\(IDF_VERSION_(MAJOR|MINOR|PATCH) (\d+)\)", f.read_text())) if f.exists() else {}
        if len(nums) == 3:
            return "v{MAJOR}.{MINOR}.{PATCH}".format(**nums)
        return git_out(path, "describe", "--tags", "--exact-match")
    if name == "emsdk":
        f = path / "upstream" / "emscripten" / "emscripten-version.txt"
        return f.read_text().strip().strip('"') if f.exists() else ""
    if name == "circuitpython":
        # The tag the checkout is exactly at; CircuitPython's own build reads
        # its version the same way, from git describe.
        return git_out(path, "describe", "--tags", "--exact-match")
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
            run([BASH, "-c", f"cd {dest} && ./install.sh all"])
        elif name == "emsdk":
            run(["git", "clone", "-q", url, str(dest)])
            run([BASH, "-c", f"cd {dest} && ./emsdk install {version} && ./emsdk activate {version}"])
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
    return sorted(p.name for p in (MP / "ports" / port / "boards").iterdir() if (p / "board.json").exists())


def board_dir(port, board):
    """Always upstream's: a board of ours is a variant of one of them."""
    return MP / "ports" / port / "boards" / board


def board_variant_dir(port, board, variant):
    """Ours, variants/<port>/<BOARD>/<VARIANT>/, when it holds a variant file."""
    d = VARIANTS_DIR / port / board / variant if board and variant else None
    return d if d and (d / "mpconfigvariant.cmake").exists() else None


def variants(port, board):
    if board:
        found = set()
        for f in board_dir(port, board).glob("mpconfigvariant_*.*"):
            found.add(f.stem[len("mpconfigvariant_"):])
        found |= {d.name for d in (VARIANTS_DIR / port / board).glob("*") if board_variant_dir(port, board, d.name)}
        return sorted(found)
    found = {p.name for p in (MP / "ports" / port / "variants").iterdir() if p.is_dir()}
    found |= {p.name for p in (VARIANTS_DIR / port).glob("*") if p.is_dir()}
    return sorted(found)


def our_variant_dir(port, variant):
    if not variant:
        return None
    d = VARIANTS_DIR / port / variant
    return d if d.is_dir() else None


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
            # modules/all/manifest.py's rule: every module but the opt-in ones.
            dirs += [MODULES_DIR / n for n in module_choices() if n != "all" and not (MODULES_DIR / n / "OPT_IN").is_file()]
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
        rows[module_name(d)] = {"path": str(real), "revision": rev, "commit": commit, "dirty": dirty}
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
            f"# the partition table: {table.name}",
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


def esp32_table(chip, board, variant):
    """Our partition table, by the same convention as the fragments, the most
    specific one found winning; None keeps the board's own."""
    found = [VARIANTS_DIR / "esp32" / board / variant / "partitions.csv"] if variant else []
    found += [VARIANTS_DIR / "esp32" / f"partitions.{chip}.csv", VARIANTS_DIR / "esp32" / "partitions.csv"]
    return next((t.resolve() for t in found if t.is_file()), None)


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
        # One of ours: its mpconfigvariant.cmake includes the upstream variant it builds on.
        base_variant = board_variant_dir("esp32", board, variant)
        if not base_variant:
            die(f"{board} has no variant {variant}")
        base_variant = base_variant / "mpconfigvariant.cmake"
    vlines.append(f"include({base_variant.as_posix()}{'' if variant else ' OPTIONAL'})")
    vlines.append(f"list(APPEND SDKCONFIG_DEFAULTS {fragment_path.as_posix()})")
    (gen / vfile).write_text("\n".join(vlines) + "\n")


def autosize_headroom(flash_bytes):
    """Room left above an image that autosize grew to fit, so a few KB of growth
    doesn't move the filesystem again: 1/32 of the flash, 64 KB to 256 KB."""
    return max(0x10000, min(0x40000, (flash_bytes // 32) & ~0xFFFF))


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

    flash = re.search(r'^CONFIG_ESPTOOLPY_FLASHSIZE="(\d+)MB"$', sdk, re.M)
    align, headroom = 0x10000, autosize_headroom(int(flash.group(1)) * 1024 * 1024 if flash else 0x400000)
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


def rp2_autosize(log_text):
    """MICROPY_HW_FLASH_STORAGE_BYTES that leaves the firmware room to fit, from
    the linker's memory table; None if the log isn't a FLASH overflow."""
    if not re.search(r"region `FLASH' overflowed", log_text):
        return None
    units = {"B": 1, "KB": 1024, "MB": 1024 ** 2, "GB": 1024 ** 3}
    regions = {}
    for name, used, uu, size, su in re.findall(
            r"^\s*(FLASH\w*):\s+(\d+(?:\.\d+)?) (B|KB|MB|GB)\s+(\d+(?:\.\d+)?) (B|KB|MB|GB)", log_text, re.M):
        regions[name] = (int(float(used) * units[uu]), int(float(size) * units[su]))
    if "FLASH" not in regions or "FLASH_FS" not in regions:
        return None
    used = regions["FLASH"][0]
    total = regions["FLASH"][1] + regions["FLASH_FS"][1]  # FLASH_ROMFS keeps its own size
    align, headroom = 0x10000, autosize_headroom(total)
    firmware = (used + headroom + align - 1) & ~(align - 1)
    storage = total - firmware
    return storage if storage >= align else None


# ---- CircuitPython: the second interpreter ---------------------------------

CP = DEPS / "circuitpython"
CP_VENV = DEPS / "circuitpython-venv"
# Ours, laid onto that checkout before a build: patches to CircuitPython's
# source, and board definitions CircuitPython doesn't have.
CP_PATCHES = REPO / "patches" / "circuitpython"
CP_BOARDS = REPO / "boards" / "circuitpython"


def lock_beside(path):
    """One build at a time in a checkout: held until this process exits."""
    lock = open(path.parent / f".{path.name}-build.lock", "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        say(f"waiting for another build to release {lock.name}")
        fcntl.flock(lock, fcntl.LOCK_EX)
    return lock


def ensure_circuitpython():
    """deps/circuitpython at its deps.lock tag, and a venv with its build tools.

    Always a checkout of our own, never a link to a sibling circuitpython/: a
    sibling is at whatever version its owner needs, and may carry other
    repositories' patches. Only the submodules a target needs are fetched,
    by CircuitPython's own tools/ci_fetch_deps.py, at build time."""
    url, version = {row[0]: row[1:] for row in read_lock("deps.lock")}["circuitpython"]
    if not CP.exists():
        DEPS.mkdir(exist_ok=True)
        clone_at(url, version, CP)
    have = dep_version("circuitpython", CP)
    if have != version:
        die(f"deps/circuitpython is {have or 'not at a tag'}, but deps.lock wants {version}")
    # The short BUILD= links (below) are symlinks, which CircuitPython's
    # .gitignore (build-*/, directories only) doesn't cover; without this the
    # checkout reads as modified to anything that asks git.
    exclude = CP / ".git" / "info" / "exclude"
    excluded = exclude.read_text() if exclude.exists() else ""
    if (CP / ".git").is_dir() and "ports/*/build-*" not in excluded:
        exclude.parent.mkdir(exist_ok=True)
        with open(exclude, "a") as f:
            f.write("\n# build_mp.py's BUILD= links\nports/*/build-*\n")
    # CircuitPython's make runs python3 from PATH and needs its dev requirements
    # (cascadetoml, jinja2, typer, ...). The venv is redone when they change.
    reqs = CP / "requirements-dev.txt"
    stamp = CP_VENV / "requirements-dev.txt"
    if not (stamp.exists() and stamp.read_text() == reqs.read_text()):
        say(f"setting up {CP_VENV.relative_to(REPO)} with CircuitPython's requirements-dev.txt")
        run([sys.executable, "-m", "venv", "--clear", str(CP_VENV)])
        run([str(CP_VENV / "bin" / "pip"), "install", "-q", "-r", str(reqs)])
        shutil.copy(reqs, stamp)
    return CP


def prepare_circuitpython(cp):
    """Apply patches/circuitpython/ and link boards/circuitpython/<port>/<board>
    into the checkout, once each. The checkout is always our own (see
    ensure_circuitpython), so changing it changes nobody else's tree. Returns
    the patch names, for the build record."""
    applied = []
    for patch in sorted(CP_PATCHES.glob("*.patch")):
        already = subprocess.run(["git", "-C", str(cp), "apply", "--reverse", "--check", str(patch)],
                                 capture_output=True).returncode == 0
        if not already:
            if subprocess.run(["git", "-C", str(cp), "apply", str(patch)]).returncode != 0:
                die(f"{patch.relative_to(REPO)} does not apply to deps/circuitpython")
            say(f"applied {patch.relative_to(REPO)}")
        applied.append(patch.name)
    for ours in sorted(p for p in CP_BOARDS.glob("*/*") if (p / "mpconfigboard.mk").is_file()):
        dest = cp / "ports" / ours.parent.name / "boards" / ours.name
        if dest.is_symlink():
            if dest.resolve() == ours.resolve():
                continue
            dest.unlink()
        elif dest.exists():
            die(f"CircuitPython already has a board called {ours.name}; rename ours")
        dest.symlink_to(ours, target_is_directory=True)
    return applied


def cp_ports():
    # zephyr-cp builds through west and never reads USER_C_MODULES.
    return sorted(p.name for p in (CP / "ports").iterdir() if (p / "Makefile").exists() and p.name != "zephyr-cp")


def cp_boards(port):
    d = CP / "ports" / port / "boards"
    return sorted(p.parent.name for p in d.glob("*/mpconfigboard.mk")) if d.is_dir() else []


def cp_variants(port):
    d = CP / "ports" / port / "variants"
    return sorted(p.name for p in d.iterdir() if (p / "mpconfigvariant.mk").exists()) if d.is_dir() else []


def module_name(d):
    """A module's own name: its modules/ entry, or for a path the repository
    it is a checkout of. A worktree's directory is named for its branch, so
    two modules from worktrees called alike would otherwise share a name."""
    real = d.resolve()
    if d.parent == MODULES_DIR:
        return d.name
    top = git_out(real, "rev-parse", "--show-toplevel")
    common = git_out(real, "rev-parse", "--path-format=absolute", "--git-common-dir")
    if top and Path(top).resolve() == real and Path(common).name == ".git":
        return Path(common).parent.name
    return real.name


def cp_user_c_modules(module_dirs, link_dir):
    """The directory holding each module's micropython.mk, for USER_C_MODULES.

    CircuitPython's py/py.mk carries MicroPython 1.29's USER_C_MODULES block,
    so the micropython.mk our MicroPython builds use is the one it reads.
    Python is not frozen here yet: CircuitPython freezes through its own
    FROZEN_MPY_DIRS, not manifest.py, so a module without C is refused rather
    than silently left out.

    Each is given to make through a link named for the module, in link_dir:
    py.mk puts a module's objects under BUILD/<its directory's name>/, so two
    worktrees both called cp-phase1 would build into one place."""
    found_dirs = []
    for d in module_dirs:
        real = d.resolve()
        found = next((c for c in (real, real / "code") if (c / "micropython.mk").is_file()), None)
        if not found:
            die(f"module '{d.name}' has no micropython.mk; CircuitPython builds take C modules only, so far")
        found_dirs.append((module_name(d), found))
    names = [n for n, _ in found_dirs]
    if len(set(names)) != len(names):
        die(f"two modules with one name: {', '.join(sorted(n for n in names if names.count(n) > 1))}")
    if link_dir.exists():
        shutil.rmtree(link_dir)
    link_dir.mkdir(parents=True)
    dirs = []
    for name, found in found_dirs:
        (link_dir / name).symlink_to(found, target_is_directory=True)
        dirs.append(link_dir / name)
    return dirs


def cp_espressif_env(port_dir, board):
    """What CircuitPython's espressif port needs before make: its ESP-IDF's
    submodules, compilers and Python packages. Returns the shell prefix that
    puts them on the path, with our venv still first.

    CircuitPython carries its own ESP-IDF (ports/espressif/esp-idf, fetched by
    ci_fetch_deps.py with the board's other submodules). Left alone, ESP-IDF's
    CMake initialises that tree's own submodules with their whole history, and
    esp32-wifi-lib alone is gigabytes; a shallow update first takes under a
    minute. The compilers go where every ESP-IDF puts them (~/.espressif, or
    IDF_TOOLS_PATH), shared with any other ESP-IDF of the same version. ESP-IDF's
    Python packages go into our CircuitPython venv, pinned by the constraints
    file its install.sh fetched, so nothing else's Python environment changes."""
    idf = port_dir / "esp-idf"
    mk = (port_dir / "boards" / board / "mpconfigboard.mk").read_text()
    target = re.search(r"^IDF_TARGET\s*=\s*(\S+)", mk, re.M)
    if not target:
        die(f"{board}'s mpconfigboard.mk names no IDF_TARGET")
    run(["git", "-C", str(idf), "submodule", "update", "--init", "--depth", "1", "--recursive", "-q"])
    run([BASH, "-c", f'cd "{idf}" && ./install.sh {target.group(1)}'], stdout=subprocess.DEVNULL)
    version = (idf / "tools" / "cmake" / "version.cmake").read_text()
    major, minor = (re.search(rf"IDF_VERSION_{k} (\d+)", version).group(1) for k in ("MAJOR", "MINOR"))
    tools = Path(os.environ.get("IDF_TOOLS_PATH", Path.home() / ".espressif"))
    constraints = tools / f"espidf.constraints.v{major}.{minor}.txt"
    reqs = idf / "tools" / "requirements" / "requirements.core.txt"
    stamp = CP_VENV / f"esp-idf-{major}.{minor}-requirements.core.txt"
    if not (stamp.exists() and stamp.read_text() == reqs.read_text()):
        say(f"installing ESP-IDF {major}.{minor}'s Python requirements into {CP_VENV.relative_to(REPO)}")
        run([str(CP_VENV / "bin" / "pip"), "install", "-q", "-r", str(reqs)]
            + (["-c", str(constraints)] if constraints.exists() else []))
        shutil.copy(reqs, stamp)
    return f'. "{idf}/export.sh" >/dev/null && export PATH="{CP_VENV / "bin"}:$PATH" && '


def build_dir_for(build, want, clean):
    """The build dir for a target, emptied first if it was built with
    something other than `want` (another module set, flash size, ...)."""
    rec_path = build / "pydevices-build.json"
    if clean and build.exists():
        shutil.rmtree(build)
        say(f"--clean: removed {build}")
    if rec_path.exists():
        had = json.loads(rec_path.read_text())
        if {k: had.get(k) for k in want} != want:
            shutil.rmtree(build)
            flash = f" (flash {had.get('flash')})" if "flash" in want else ""
            args = ("" if "make_args" not in want else " (make arguments not recorded)" if "make_args" not in had
                    else f" (make {' '.join(had['make_args']) or 'with no arguments'})")
            say(f"{build} was built with {had.get('modules')!r} {had.get('module_set') or ''}{flash}{args}; "
                f"wiped, building {want['modules']!r} {want['module_set']}"
                + (f" (make {' '.join(want['make_args']) or 'with no arguments'})" if args else ""))
    elif build.exists() and any(build.iterdir()):
        # Something built here without a record: nobody can say with what.
        shutil.rmtree(build)
        say(f"{build} has no build record; wiped")
    build.mkdir(parents=True, exist_ok=True)
    # Written before the build, so a build that fails still says what it was
    # built with, and the next one with a different set wipes it.
    rec_path.write_text(json.dumps(dict(want, complete=False), indent=1) + "\n")
    return rec_path


def build_circuitpython(args, make_extra, ws):
    if os.name == "nt":
        die("--interpreter circuitpython builds from Linux or WSL")
    if args.flash:
        die("--flash is for MicroPython's esp32 port")
    ensure_modules(ws)
    cp = ensure_circuitpython()
    lock = lock_beside(cp)  # noqa: F841 -- held until exit
    cp_patches = prepare_circuitpython(cp)

    port = args.port or choose("CircuitPython port:", cp_ports())
    if port not in cp_ports():
        die(f"no CircuitPython port '{port}' (have: {', '.join(cp_ports())})")
    board, variant = args.board, args.variant
    if cp_boards(port):
        board = board or choose("Board:", cp_boards(port))
        if board not in cp_boards(port):
            die(f"no board '{board}' for CircuitPython's {port}")
        if variant:
            die("CircuitPython boards have no variants; leave out --variant")
    else:
        if board:
            die(f"{port} has no boards; leave out --board")
        if variant is None and not args.port:
            variant = choose("Variant:", cp_variants(port), allow_none=True)
        if variant and variant not in cp_variants(port):
            die(f"no variant '{variant}' (have: {', '.join(cp_variants(port))})")

    spec = args.modules
    if spec is None:
        if not sys.stdin.isatty():
            die("--modules: not given, and there is no terminal to ask on")
        say("Modules: " + ", ".join(module_choices()))
        spec = input('Which (comma list; full paths for others; empty for none)? ').strip()
    spec, module_dirs = resolve_modules(spec)

    # The unix port's own default variant is coverage.
    build = OUT_DIR / "circuitpython" / port / (board or variant or "coverage")
    module_set = sorted(d.name if d.parent == MODULES_DIR else str(d) for d in module_dirs)
    # make's own arguments are part of what was built: a CIRCUITPY_X=0 left
    # out of the next build would otherwise keep its generated module table.
    want = {"interpreter": "circuitpython", "port": port, "board": board, "variant": variant,
            "modules": spec, "module_set": module_set, "make_args": make_extra}
    rec_path = build_dir_for(build, want, args.clean)
    user_c = cp_user_c_modules(module_dirs, build / "usermods")

    env = {k: v for k, v in os.environ.items()
           if k not in ("USER_C_MODULES", "FROZEN_MANIFEST", "BUILD", "BOARD", "VARIANT", "VARIANT_DIR", "PYTHONPATH")}
    env["PATH"] = os.pathsep.join([str(CP_VENV / "bin"), env.get("PATH", "")])
    env["VIRTUAL_ENV"] = str(CP_VENV)
    # Only the submodules this target needs, the way CircuitPython's CI does:
    # a board name fetches its port's, "tests" what the unix port builds with.
    run([str(CP_VENV / "bin" / "python"), "tools/ci_fetch_deps.py", board or "tests"], cwd=cp, env=env,
        stdout=subprocess.DEVNULL)
    # mpy-cross first and on its own: a port's sub-make for it would inherit
    # BUILD= from our command line (the same trap as micropython#19667).
    run(["make", "-C", str(cp / "mpy-cross"), "-j", JOBS], env=env, stdout=subprocess.DEVNULL)

    # make is given a short relative BUILD, a link in the port dir to ours: the
    # raspberrypi link rule echoes every object path in one shell argument,
    # and with our absolute build path in front of each that passes Linux's
    # 128 KB limit on a single argument ("Argument list too long").
    port_dir = cp / "ports" / port
    short = port_dir / f"build-{board or variant or 'coverage'}"
    if short.is_symlink() or short.is_file():
        short.unlink()
    elif short.is_dir():
        shutil.rmtree(short)
    short.symlink_to(build, target_is_directory=True)
    make = ["make", "-C", str(port_dir), "-j", JOBS, f"BUILD={short.name}",
            "USER_C_MODULES=" + " ".join(str(d) for d in user_c)]
    if board:
        make.append(f"BOARD={board}")
    elif variant:
        make.append(f"VARIANT={variant}")
    make += make_extra
    say(f"CircuitPython {dep_version('circuitpython', cp)} {port} {board or variant or 'coverage'}; "
        f"USER_C_MODULES: {' '.join(str(d) for d in user_c) or '(none)'}")
    prefix = cp_espressif_env(port_dir, board) if port == "espressif" else ""
    rc = subprocess.run([BASH, "-c", prefix + shlex.join(make)], env=env).returncode
    if rc != 0:
        die(f"the build failed (make exit {rc})")
    rec = dict(want, complete=True)
    rec["circuitpython"] = git_out(cp, "describe", "--tags", "--always", "--dirty")
    rec["circuitpython_patches"] = cp_patches
    rec["module_revisions"] = record(module_dirs)
    rec_path.write_text(json.dumps(rec, indent=1) + "\n")
    say(f"\nBuilt into {build}")
    for name in ("firmware.uf2", "firmware.bin", "firmware.elf", "micropython"):
        if (build / name).exists():
            say(f"  {build / name}")


# ---- the build -------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], allow_abbrev=False)
    ap.add_argument("--interpreter", choices=("micropython", "circuitpython"), default="micropython",
                    help="circuitpython: CircuitPython-compatible firmware, from CircuitPython's own ports and boards")
    ap.add_argument("--port")
    ap.add_argument("--board")
    ap.add_argument("--variant")
    ap.add_argument("--modules", help='comma list: short names, full paths, or "all"')
    ap.add_argument("--flash", help="esp32 flash size, e.g. 16MB")
    ap.add_argument("--no-autosize", action="store_true", help="esp32: refuse instead of growing the app partition")
    ap.add_argument("--clean", action="store_true", help="delete this target's build dir first")
    args, make_extra = ap.parse_known_args()

    msys2 = windows_toolchain() if os.name == "nt" else None
    if msys2:
        say(f"using MSYS2 at {msys2}")
    ws = workspace()
    if args.interpreter == "circuitpython":
        return build_circuitpython(args, make_extra, ws)
    ensure_micropython(ws)
    ensure_modules(ws)
    mp = MP.resolve()
    # One build at a time in a MicroPython checkout: preparing rewrites the
    # tree, and two esp32 builds race on the port's managed_components/. The
    # lock sits beside the checkout, so it is the one any other build tool
    # sharing that checkout takes.
    # Held until this process exits.
    lock = open(mp.parent / ".micropython-build.lock", "w")
    if fcntl:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            say(f"waiting for another build to release {lock.name}")
            fcntl.flock(lock, fcntl.LOCK_EX)
    else:
        try:
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            say(f"waiting for another build to release {lock.name}")
            while True:
                try:  # LK_LOCK gives up after ten seconds, so ask again
                    msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
                    break
                except OSError:
                    pass
    upstream = (REPO / "UPSTREAM").read_text().strip()
    apply_patches.prepare(mp, upstream, apply_patches.series(), refresh=True)

    port = args.port or choose("Port:", ports())
    if port not in ports():
        die(f"no port '{port}' (have: {', '.join(ports())})")
    if os.name == "nt":
        if port == "windows":
            check_windows_tools()
        else:
            say(f"note: on native Windows only the windows port is tested; build {port} from WSL")
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
    if port == "esp32" and flash is None and not args.port:
        # Upstream's generic tables assume 4 MB, which most module sets outgrow.
        flash = choose("Flash size (none keeps the board's own):", list(FLASH_SIZES), allow_none=True)

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
    # The set --modules resolved to, not just its spelling: "all" grows when a
    # module is added, and a build dir from before would otherwise be reused,
    # its frozen content generated against a qstr pool the new module has
    # since changed (frozen_content.c then redeclares MP_QSTR_Image, ...).
    module_set = sorted(d.name if d.parent == MODULES_DIR else str(d) for d in module_dirs)
    want = {"port": port, "board": board, "variant": variant, "flash": flash, "modules": spec,
            "module_set": module_set}
    build_dir_for(build, want, args.clean)

    env = dict(os.environ)
    for leak in ("USER_C_MODULES", "FROZEN_MANIFEST", "BUILD", "BOARD", "VARIANT", "BOARD_DIR", "VARIANT_DIR"):
        env.pop(leak, None)
    env["PYDEVICES_MODULES"] = spec
    if os.name == "nt":
        # Upstream's tools open sources without encoding=; Windows' default is cp1252.
        env.setdefault("PYTHONUTF8", "1")
    # A prebuilt mpy-cross, named in the environment: neither make nor CMake
    # then runs the mpy-cross sub-make that would inherit BUILD= (micropython#19667).
    # Windows has no python3: give make this Python, in a form MSYS sh keeps.
    winpy = [f"PYTHON={Path(sys.executable).as_posix()}"] if os.name == "nt" else []
    run(["make", "-C", str(mp / "mpy-cross"), "-j", JOBS, *winpy], env={k: v for k, v in env.items() if k != "PYDEVICES_MODULES"},
        stdout=subprocess.DEVNULL)
    exe = ".exe" if os.name == "nt" else ""
    env["MICROPY_MPYCROSS"] = (mp / "mpy-cross" / "build" / f"mpy-cross{exe}").as_posix()

    port_dir = mp / "ports" / port
    make = ["make", "-C", str(port_dir), "-j", JOBS, f"BUILD={build.as_posix()}", *winpy]
    prefix = ""
    ours = our_variant_dir(port, variant) if not board else None
    manifest = ours / "manifest.py" if ours and (ours / "manifest.py").exists() else MODULES_DIR / "manifest.py"
    make.append(f"FROZEN_MANIFEST={manifest.as_posix()}")
    if board:
        make.append(f"BOARD={board}")
        if variant:
            make.append(f"BOARD_VARIANT={variant}")
    elif ours:
        make.append(f"VARIANT_DIR={ours.as_posix()}")
    elif variant:
        make.append(f"VARIANT={variant}")

    if port == "esp32":
        idf = ensure_dep("esp-idf", ws)
        prefix = f'. "{idf}/export.sh" >/dev/null && '
    elif port == "webassembly":
        emsdk = ensure_dep("emsdk", ws)
        prefix = f'. "{emsdk}/emsdk_env.sh" >/dev/null 2>&1 && '
    elif port == "windows":
        if os.name != "nt":  # on Windows, MSYS2's MinGW gcc is the native compiler
            make.append("CROSS_COMPILE=x86_64-w64-mingw32-")
        if any(d.resolve().name == "displayif" for d in module_dirs):
            make.append(f"SDL2_DEV={ensure_dep('SDL2', ws).as_posix()}")
    make += make_extra

    def shell(cmd, log=None):
        line = prefix + shlex.join(cmd)
        if log is None:
            return subprocess.run([BASH, "-c", line], env=env).returncode
        p = subprocess.Popen([BASH, "-c", line], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
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
            table = esp32_table(chip, board, variant)
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
        elif port == "rp2":
            shell(make + ["submodules"])
            storage = None
            for attempt in (1, 2):
                extra = [f"MICROPY_HW_FLASH_STORAGE_BYTES={storage}"] if storage else []
                rc, out = shell(make + extra, log=True)
                if rc == 0 or attempt == 2:
                    break
                storage = rp2_autosize(out)
                if not storage:
                    break
                if args.no_autosize:
                    say(f"\nThe firmware doesn't fit. MICROPY_HW_FLASH_STORAGE_BYTES={storage} would fit it.")
                    say("(--no-autosize, so nothing was rebuilt.)")
                    break
                say(f"\nautosize: the firmware didn't fit; building once more with a {storage // 1024} KB filesystem.")
                say("  Shrinking the filesystem moves it: a board flashed with this image comes")
                say("  up with an empty filesystem (cmods#30).\n")
                # The port runs CMake only when the build has no Makefile yet.
                (build / "Makefile").unlink(missing_ok=True)
            # What the image was built with, grown now or on an earlier run (CMake caches it).
            cache = build / "CMakeCache.txt"
            got = re.search(r"^MICROPY_HW_FLASH_STORAGE_BYTES:\w+=(\d+)$", cache.read_text(), re.M) if cache.exists() else None
            rp2_storage = int(got.group(1)) if rc == 0 and got else None
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
    if port == "rp2" and rp2_storage:
        rec["rp2_storage_bytes"] = rp2_storage
    rec["micropython"] = git_out(mp, "describe", "--always", "--abbrev=12")
    rec["module_revisions"] = record(module_dirs)
    rec_path.write_text(json.dumps(rec, indent=1) + "\n")
    say(f"\nBuilt into {build}")
    for name in ("firmware.bin", "firmware.uf2", "micropython", "micropython.exe", "micropython.mjs", "micropython.wasm"):
        if (build / name).exists():
            say(f"  {build / name}")


if __name__ == "__main__":
    main()
