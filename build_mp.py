#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

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
CircuitPython at its DEPENDENCIES.lock pin, our C modules compiled in through its
USER_C_MODULES. Ports and boards are then CircuitPython's own.

This script names no version and no setting. Pins live in DEPENDENCIES.lock,
read by scripts/pins.py; esp32 settings live in sdkconfig fragments found by
convention. See docs/build-plan.md.

Environment: OUT_DIR (default builds/), VARIANTS_DIR (default variants/),
MODULES_DIR (default modules/), JOBS (default: every core), and on Windows
MSYS2_ROOT (default C:\\msys64), the MSYS2 whose make and MinGW gcc build the
windows port.
"""

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
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
sys.path.insert(0, str(REPO / "scripts"))
import apply_patches  # noqa: E402
import pins  # noqa: E402


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


def lock():
    """DEPENDENCIES.lock as {name: Pin}, read by scripts/pins.py."""
    try:
        return pins.read(REPO)
    except (pins.LockError, OSError) as e:
        die(str(e))


def pin(name):
    p = lock().get(name)
    if p is None:
        die(f"{pins.LOCK} has no {name} line")
    return p


def link(dest, target):
    dest.parent.mkdir(parents=True, exist_ok=True)
    os.symlink(os.path.relpath(target, dest.parent), dest)
    say(f"linked {dest.relative_to(REPO)} -> {target}")


def clone_at(url, ref, dest, recursive=False, commit=None):
    """A depth-1 checkout of a tag or a full commit. With commit, a tag that
    is now another commit is refused, and the checkout removed."""
    say(f"cloning {url} at {ref} into {dest.relative_to(REPO)}")
    run(["git", "init", "-q", str(dest)])
    # Git for Windows defaults core.autocrlf=true: patches checked out CRLF don't apply.
    run(["git", "-C", str(dest), "config", "core.autocrlf", "false"])
    run(["git", "-C", str(dest), "remote", "add", "origin", url])
    # A tag is fetched as a tag, so the checkout can still say which one it is.
    spec = ref if re.fullmatch(r"[0-9a-f]{40}", ref) else f"refs/tags/{ref}:refs/tags/{ref}"
    run(["git", "-C", str(dest), "fetch", "-q", "--depth", "1", "origin", spec])
    run(["git", "-C", str(dest), "-c", "advice.detachedHead=false", "checkout", "-q", ref if spec != ref else "FETCH_HEAD"])
    head = git_out(dest, "rev-parse", "HEAD")
    if commit and head != commit:
        shutil.rmtree(dest)
        die(f"{url} {ref} is {head}, but {pins.LOCK} pins {commit}")
    if recursive:
        run(["git", "-C", str(dest), "submodule", "update", "--init", "--recursive", "--depth", "1", "-q"])


def ensure_micropython(ws):
    if MP.exists():
        return
    if ws:
        link(MP, ws / "micropython")
    else:
        mp = pin(pins.MICROPYTHON)
        clone_at(mp.url, mp.ref, MP)
        fetch_history(MP)


def fetch_history(mp):
    """The commits since the release tag the checkout follows, and that tag, onto
    a depth-1 clone, so `git describe` can say which release the pinned commit
    follows. Without them the build's sys.version is a bare commit hash, and the
    image shows no version. A few seconds: it is the cycle's commits (a couple of
    hundred), not the history. (A `--unshallow --filter` fetch of everything
    hung on GitHub's runners.)

    The tag is the one mpconfig.h names: upstream tags the commit that sets the
    version, vX.Y.Z at a release and vX.Y.Z-preview at the start of a cycle."""
    if git_out(mp, "describe", "--tags", "--exact-match"):
        return  # the pin is a tag
    text = (mp / "py" / "mpconfig.h").read_text()
    n = [re.search(rf"define MICROPY_VERSION_{k} (\d+)", text) for k in ("MAJOR", "MINOR", "MICRO", "PRERELEASE")]
    if not all(n):
        say("py/mpconfig.h has no version; sys.version will show a commit")
        return
    tag = "v" + ".".join(m.group(1) for m in n[:3]) + ("-preview" if int(n[3].group(1)) else "")
    head = git_out(mp, "rev-parse", "HEAD")
    say(f"fetching MicroPython's commits since {tag}, for its version")
    # --shallow-exclude stops at the tag's child; one more level reaches the tag's commit.
    for cmd in (["--shallow-exclude=" + tag, head], ["--deepen=1", head], [f"refs/tags/{tag}:refs/tags/{tag}"]):
        if subprocess.run(["git", "-C", str(mp), "fetch", "-q", "origin", *cmd]).returncode != 0:
            say(f"could not fetch {tag}; sys.version will show a commit")
            return


def micropython_version(mp, upstream):
    """(MICROPY_GIT_TAG, MICROPY_GIT_HASH) for a build of the overlay on `upstream`:
    what upstream's own build would print at that commit, v1.30.0-preview.160.gb70ae1aca6.

    The version names the upstream commit, not the local overlay commit on top
    of it, which no one else has and git cannot find. The overlay is part of
    what this repository is, and pydevices-build.json records which patches a
    build carries, so it does not make the build "dirty"; only edits beyond it
    do (a submodule carrying our recorded patches doesn't count either)."""
    described = git_out(mp, "describe", "--tags", "--match", "v[1-9].*", "--abbrev=10", upstream)
    if not described:
        say("MicroPython has no tags here; sys.version will show a commit, not a version")
        return None
    parts = described.split("-", 1)
    tag = parts[0] + ("-" + parts[1].replace("-", ".") if len(parts) > 1 else "")
    if git_out(mp, "status", "--porcelain", "--untracked-files=no", "--ignore-submodules=all"):
        tag += ".dirty" if "-" in tag else "-dirty"
    return tag, git_out(mp, "rev-parse", "--short=10", upstream)


# omnipython-lvgl is pinned in DEPENDENCIES.lock beside the modules, but it is not
# one module directory: it is LVGL, its generated bindings and one port per
# interpreter (ports/micropython, ports/circuitpython), which read generated/,
# lv_conf.h and lvgl/ from the repository's root. The checkout lands in
# modules/.omnipython-lvgl (the dot keeps it out of the module list): in the
# workspace a link to the sibling checkout, which wins like every other module,
# anywhere else a sparse fetch at the pinned commit. --modules names it `lvgl`,
# and "all" takes it too; a build gets the port for its interpreter.
LVGL_REPO = "omnipython-lvgl"
LVGL_MODULE = "lvgl"
# The names it went by before the four LVGL repositories became one.
LVGL_OLD_NAMES = ("lvgl-micropython", "lvgl-circuitpython", "lvgl-bindings")


def ensure_modules(ws):
    for name, url, commit, _ in (p for p in lock().values() if p.group == "modules"):
        dest = MODULES_DIR / name
        if dest.exists() or dest.is_symlink():
            continue
        if ws and (ws / name).is_dir():
            link(dest, ws / name)
        else:
            clone_at(url, commit, dest)


def refuse_old_lvgl_names(names):
    """lvgl-micropython, lvgl-circuitpython and lvgl-bindings are one repository
    now, omnipython-lvgl, named `lvgl` here whatever the interpreter."""
    old = [n for n in names if n in LVGL_OLD_NAMES]
    if old:
        die(f"{', '.join(old)}: LVGL is one module now, {LVGL_MODULE} (the {LVGL_REPO} repository), "
            f"for MicroPython and CircuitPython alike: --modules ...,{LVGL_MODULE}")


def lvgl_root_of(d):
    """The omnipython-lvgl checkout `d` is, or is a port directory of, else None."""
    real = Path(d).resolve()
    if (real / "lv_conf.h").is_file() and (real / "ports" / "micropython").is_dir():
        return real
    root = real.parent.parent
    if real.parent.name == "ports" and (root / "lv_conf.h").is_file():
        return root
    return None


def lvgl_port(root, interpreter):
    return (Path(root) / "ports" / interpreter).resolve()


def lvgl_checkout(ws):
    """modules/.omnipython-lvgl: LVGL, its bindings and its ports, at their
    DEPENDENCIES.lock commit (or the workspace's own checkout).

    Fetched shallow, sparse and blobless: the ports, generated/, python/ and
    the root's files (lv_conf.h), and LVGL's src/ at the commit the repository
    records (and its include/, from LVGL 9.6), so it downloads tens of MB
    rather than LVGL's whole history."""
    url, commit = pin(LVGL_REPO)[1:3]
    dest = MODULES_DIR / f".{LVGL_REPO}"
    if dest.is_symlink():
        if (dest / "lv_conf.h").is_file():
            return dest.resolve()
        dest.unlink()
    elif dest.is_dir():
        if git_out(dest, "rev-parse", "HEAD") == commit and (dest / "lvgl" / "lvgl.h").is_file():
            return dest.resolve()
        say(f"{dest} is not at {pins.LOCK}'s {commit[:12]}; fetching it again")
        shutil.rmtree(dest)
    sibling = ws / LVGL_REPO if ws else None
    if sibling and (sibling / "lv_conf.h").is_file():
        if not (sibling / "lvgl" / "lvgl.h").is_file():
            die(f"{sibling} has no LVGL: git -C {sibling} submodule update --init --depth 1 lvgl")
        link(dest, sibling)
        return dest.resolve()
    say(f"fetching {url} at {commit[:12]} into {dest} (the ports, generated/, lv_conf.h and LVGL's src/)")
    part = dest.with_name(dest.name + ".partial")
    shutil.rmtree(part, ignore_errors=True)

    def sparse(repo, origin, sha, paths):
        repo.mkdir(parents=True)
        for cmd in (["init", "-q"], ["config", "core.autocrlf", "false"], ["remote", "add", "origin", origin],
                    ["config", "extensions.partialClone", "origin"], ["sparse-checkout", "set", *paths],
                    ["fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", sha],
                    ["-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD"]):
            r = subprocess.run(["git", "-C", str(repo), *cmd], text=True, capture_output=True, check=False)
            if r.returncode:
                shutil.rmtree(part, ignore_errors=True)
                die(f"could not fetch {origin} at {sha} (git {cmd[0]}: {r.stderr.strip() or r.returncode});"
                    f" is {pins.LOCK}'s {LVGL_REPO} commit on GitHub?")

    sparse(part, url, commit, ["generated", "lvgl", "ports", "python"])
    lvgl_sha = git_out(part, "ls-tree", "HEAD", "lvgl").split()[2:3]
    lvgl_url = git_out(part, "config", "-f", ".gitmodules", "submodule.lvgl.url")
    if not lvgl_sha or not lvgl_url:
        shutil.rmtree(part, ignore_errors=True)
        die(f"{LVGL_REPO} {commit[:12]} records no lvgl submodule")
    shutil.rmtree(part / "lvgl")
    # LVGL 9.6 keeps its public headers in include/, beside src/.
    sparse(part / "lvgl", lvgl_url, lvgl_sha[0], ["include", "src"])
    part.rename(dest)
    return dest.resolve()


# omnipython-lib is the repository of our Python-only packages: pydevices,
# palettes, pdwidgets, appkit, audioinstruments, audioeffects, and every board
# config and driver. It is pinned in DEPENDENCIES.lock beside the modules but is not
# one: a build asks its tools/libbuild.py for the library tree, one directory
# per package with a manifest.py beside its sources, and registers that tree
# with add_library(). A manifest then require()s a package from it. Packages
# with C stay modules.
#
# The checkout lands in modules/.omnipython-lib (the dot keeps it out of
# "all"): in the workspace a link to the sibling checkout, which wins like
# every other module, anywhere else a clone at the pinned commit. The tree is
# assembled into deps/omnipython-lib on every build that needs it; the outside
# packages the library pins by tag (pygraphics, audioinstruments, audioeffects) are
# cloned once into deps/omnipython-lib-sources. In the workspace those come
# from the sibling checkouts instead, the way every module does, so a
# workspace build freezes the code beside it rather than the pinned tag.
LIBRARY = "omnipython-lib"


def library_checkout(ws):
    """modules/.omnipython-lib: the library repository, at its DEPENDENCIES.lock commit
    (or the workspace's own checkout of it)."""
    url, commit = pin(LIBRARY)[1:3]
    dest = MODULES_DIR / f".{LIBRARY}"
    if dest.is_symlink():
        if (dest / "tools" / "libbuild.py").is_file():
            return dest
        dest.unlink()
    elif dest.is_dir():
        if git_out(dest, "rev-parse", "HEAD") == commit:
            return dest
        say(f"{dest} is not at {pins.LOCK}'s {commit[:12]}; fetching it again")
        shutil.rmtree(dest)
    # Called pydevices until the rename; either name is the same repository.
    for sibling in ((ws / LIBRARY, ws / "pydevices") if ws else ()):
        if (sibling / "tools" / "libbuild.py").is_file():
            link(dest, sibling)
            return dest
    clone_at(url, commit, dest)
    return dest


class Library:
    """The library tree, assembled the first time a build asks for it."""

    def __init__(self, ws):
        self.ws = ws
        self.source = None
        self.tree = None

    def build(self):
        if self.tree is None:
            self.source = library_checkout(self.ws)
            out = DEPS / LIBRARY
            DEPS.mkdir(exist_ok=True)
            say(f"assembling the {LIBRARY} tree from {self.source.resolve()}")
            sources = []
            for name, sibling in self.sibling_sources():
                say(f"  {name} from the sibling checkout {sibling}, not its pinned tag")
                sources += ["--source", f"{name}={sibling}"]
            r = subprocess.run(
                [sys.executable, str(self.source / "tools" / "libbuild.py"), "build", "--tree-only",
                 "--root", str(self.source), "--out", str(out), "--cache", str(DEPS / f"{LIBRARY}-sources"),
                 "--library-name", LIBRARY, *sources], text=True, capture_output=True)
            if r.returncode != 0:
                die(f"{self.source / 'tools' / 'libbuild.py'} failed:\n{r.stdout}{r.stderr}")
            self.tree = out / "library"
        return self.tree

    def sibling_sources(self):
        """[(package, checkout)] for each package the library's lib-lock.json pins
        from another repository that the workspace has a checkout of."""
        if not self.ws:
            return []
        try:
            pinned = json.loads((self.source / "lib-lock.json").read_text())["pinned"]
        except (OSError, ValueError, KeyError):
            return []
        out = []
        for name, row in sorted(pinned.items()):
            sibling = self.ws / row["repository"].rsplit("/", 1)[-1]
            if (sibling / "lib" / name).is_dir():
                out.append((name, sibling))
        return out

    def names(self):
        """Every package in the tree."""
        return {p.name for p in self.build().iterdir() if (p / "manifest.py").is_file()}


def uses_library(module_dirs, manifest):
    """Whether this build's manifests ask for the library: a module's, a path
    module's, or the variant's own. They say so by naming it."""
    files = [d / "manifest.py" for d in module_dirs] + [manifest]
    return any(f.is_file() and LIBRARY in f.read_text() for f in files)


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
    if name in CP_ESP_IDF_COMPONENT_DEPS:
        # An ESP-IDF component's own manifest says its version.
        f = path / "idf_component.yml"
        m = re.search(r"^version:\s*['\"]?([^'\"\s]+)", f.read_text(), re.M) if f.exists() else None
        return m.group(1) if m else ""
    if name == "SDL2":
        h = path / "x86_64-w64-mingw32" / "include" / "SDL2" / "SDL_version.h"
        if not h.exists():
            return ""
        nums = dict(re.findall(r"#define SDL_(MAJOR_VERSION|MINOR_VERSION|PATCHLEVEL)\s+(\d+)", h.read_text()))
        return "{MAJOR_VERSION}.{MINOR_VERSION}.{PATCHLEVEL}".format(**nums)
    return ""


def ensure_dep(name, ws):
    """Fetch (or link) one toolchain and refuse if it isn't the locked version:
    the tag a checkout is pinned by, or a download's version."""
    p = pin(name)
    url, version = p.url, p.tag or p.ref
    dest = DEPS / name
    if not (dest.exists() or dest.is_symlink()):
        if ws and (ws / name).is_dir():
            link(dest, ws / name)
        elif name == "esp-idf":
            clone_at(url, version, dest, recursive=True, commit=p.ref)
            run([BASH, "-c", f"cd {dest} && ./install.sh all"])
        elif name == "emsdk":
            clone_at(url, version, dest, commit=p.ref)
            run([BASH, "-c", f"cd {dest} && ./emsdk install {version} && ./emsdk activate {version}"])
        elif name == "SDL2":
            DEPS.mkdir(exist_ok=True)
            with tempfile.TemporaryDirectory() as tmp:
                archive = Path(tmp) / "sdl2.tar.gz"
                say(f"downloading {url}")
                urllib.request.urlretrieve(url, archive)
                with tarfile.open(archive) as t:
                    # Python 3.12 and 3.13 warn, on every build, that 3.14 will
                    # filter archives by default; ask for that filter where it exists.
                    t.extractall(tmp, **({"filter": "data"} if hasattr(tarfile, "data_filter") else {}))
                top = next(p for p in Path(tmp).iterdir() if p.is_dir())
                shutil.move(str(top), str(dest))
        elif url.endswith(".zip"):
            # An ESP-IDF component from Espressif's registry: its files sit at
            # the archive's top level.
            DEPS.mkdir(exist_ok=True)
            with tempfile.TemporaryDirectory(dir=DEPS) as tmp:
                archive = Path(tmp) / "component.zip"
                say(f"downloading {url}")
                urllib.request.urlretrieve(url, archive)
                with zipfile.ZipFile(archive) as z:
                    z.extractall(Path(tmp) / name)
                shutil.move(str(Path(tmp) / name), str(dest))
    have = dep_version(name, dest.resolve())
    if have != version:
        die(f"deps/{name} is {have or 'unreadable'}, but {pins.LOCK} wants {version}")
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
    """Ours, variants/<port>/<BOARD>/<VARIANT>/, when it holds a variant file
    (mpconfigvariant.cmake on a CMake port, mpconfigvariant.mk on stm32)."""
    d = VARIANTS_DIR / port / board / variant if board and variant else None
    found = d and any((d / f).exists() for f in ("mpconfigvariant.cmake", "mpconfigvariant.mk"))
    return d if found else None


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


def frozen_manifest(port, board, variant):
    """The manifest a build freezes. A variant of ours, of a port or of a
    board, may carry a manifest.py that includes modules/manifest.py and adds
    what only that variant freezes; every other build freezes
    modules/manifest.py itself."""
    mine = board_variant_dir(port, board, variant) if board else our_variant_dir(port, variant)
    if mine and (mine / "manifest.py").exists():
        return mine / "manifest.py"
    return MODULES_DIR / "manifest.py"


def board_manifest(port, board):
    """Upstream's own manifest for the board, if it has one: what the board
    freezes beyond its port (the GIGA's networking bundle, for one).
    modules/manifest.py includes it in place of the port-wide manifest it
    starts from."""
    own = board_dir(port, board) / "manifest.py" if board else None
    return own if own is not None and own.exists() else None


def module_choices():
    return sorted(p.name for p in MODULES_DIR.iterdir() if p.is_dir() and not p.name.startswith("."))


def resolve_modules(spec, library=None, interpreter="micropython", lvgl=None):
    """The comma list as modules/manifest.py will read it (paths made
    absolute), the module directories it amounts to ("all" expanded), which
    the esp32 conventions and the build record need, and the library packages
    it names. A name that is no module of ours but a package of the library is
    required from it; `library` is the Library to ask, built only then.

    `lvgl`, and "all", take omnipython-lvgl's port for `interpreter`, as does a
    path to an omnipython-lvgl checkout; `lvgl` is called for the checkout
    only then, so a build without LVGL fetches none. Without `lvgl`, "all"
    leaves LVGL out and the name `lvgl` is refused."""
    names, dirs, packages = [], [], []
    refuse_old_lvgl_names([Path(s.strip()).name for s in spec.split(",")])

    def lvgl_dir(root=None):
        if root is None:
            if lvgl is None:
                die(f"{LVGL_MODULE}: no {LVGL_REPO} checkout to build it from")
            root = lvgl()
        return lvgl_port(root, interpreter)

    for item in (s.strip() for s in spec.split(",")):
        if not item:
            continue
        if "/" in item or os.sep in item:
            path = Path(item).expanduser().resolve()
            if not path.is_dir():
                die(f"module path does not exist: {item}")
            root = lvgl_root_of(path)
            if root and path != lvgl_dir(root):
                if path != root:
                    die(f"{item} is {LVGL_REPO}'s port for another interpreter; this build takes "
                        f"{lvgl_dir(root)} (or name it {LVGL_MODULE})")
                path = lvgl_dir(root)
            names.append(str(path))
            dirs.append(path)
        elif item == "all":
            names.append("all")
            # modules/all/manifest.py's rule: every module but the opt-in ones.
            dirs += [MODULES_DIR / n for n in module_choices() if n != "all" and not (MODULES_DIR / n / "OPT_IN").is_file()]
            # and LVGL, which modules/all/manifest.py finds through $PYDEVICES_LVGL.
            if lvgl is not None:
                dirs.append(lvgl_dir())
        elif item == LVGL_MODULE and not (MODULES_DIR / item).is_dir():
            path = lvgl_dir()
            names.append(str(path))
            dirs.append(path)
        elif (MODULES_DIR / item).is_dir():
            names.append(item)
            dirs.append(MODULES_DIR / item)
        elif library and item in library.names():
            names.append(item)
            packages.append(item)
        else:
            have = module_choices() + [LVGL_MODULE] + (sorted(library.names()) if library else [])
            die(f"no module '{item}' in {MODULES_DIR} (have: {', '.join(sorted(have))})")
    unique = []
    for d in dirs:
        if d.resolve() not in [u.resolve() for u in unique]:
            unique.append(d)
    return ",".join(names), unique, sorted(set(packages))


def lvgl_in(module_dirs):
    """The omnipython-lvgl checkout a build compiles, or None when it has no LVGL."""
    return next((r for r in (lvgl_root_of(d) for d in module_dirs) if r), None)


def record(dirs, library=None):
    """A row per module (omnipython-lvgl's under its repository's name) and one
    for the omnipython-lib the build's library packages came from: which commit
    went in, and whether it had uncommitted changes."""
    rows = {}
    named = [(module_name(d), d) for d in dirs] + ([(LIBRARY, library)] if library else [])
    for name, d in named:
        real = d.resolve()
        rev = git_out(real, "describe", "--always", "--dirty", "--abbrev=12") or "(not a git checkout)"
        # The commit and whether anything was uncommitted: what a consumer
        # needs to tell later whether the code that went in has moved.
        commit = git_out(real, "rev-parse", "HEAD") or None
        dirty = bool(git_out(real, "status", "--porcelain", "--untracked-files=no")) if commit else None
        # Which uncommitted changes, so the next build can tell an edit from
        # the same edit built again.
        changes = None
        if dirty:
            diff = subprocess.run(["git", "-C", str(real), "diff", "HEAD", "--binary"], capture_output=True).stdout
            changes = hashlib.sha1(diff).hexdigest()[:12]
        rows[name] = {"path": str(real), "revision": rev, "commit": commit, "dirty": dirty,
                                "changes": changes}
        off = off_pin(name, real, commit)
        if off:
            rows[name]["lock"] = off
    return rows


def off_pin(name, real, commit):
    """How a module checkout stands against its DEPENDENCIES.lock pin when it isn't
    at it (and a line saying so), else None. In the workspace a sibling
    checkout wins over the pin, so one left behind builds old code into an
    image named for the current commit; this line is how you find out."""
    p = lock().get(name)
    want = p.ref if p and not p.download else None
    if not want or not commit or commit == want:
        return None

    def git_ok(*a):
        return subprocess.run(["git", "-C", str(real), *a], capture_output=True).returncode == 0

    if not git_ok("cat-file", "-e", want + "^{commit}"):
        where = "not at (the pin isn't fetched here)"
    elif git_ok("merge-base", "--is-ancestor", want, commit):
        where = "ahead of"
    elif git_ok("merge-base", "--is-ancestor", commit, want):
        where = "behind"
    else:
        where = "beside"
    say(f"{name}: building {real} at {commit[:12]}, {where} {pins.LOCK}'s {want[:12]}"
        + ("; git -C " + str(real) + " pull to catch up" if where == "behind" else ""))
    return f"{where} {want[:12]}"


def modules_moved(had, now):
    """What has moved in the modules since a build dir was built: a line per
    module whose commit differs between the build record's module_revisions
    (`had`) and record()'s rows for this build (`now`), or [] when nothing has.

    Only the commit counts. Uncommitted edits to a module rebuild incrementally,
    the way make and ninja already handle them: wiping on each edit would make
    every change to a sibling module a full esp32 rebuild.

    make alone doesn't catch a moved module. Its C side can change the qstr
    pool, and the frozen content and qstr headers generated against the old
    pool are reused (frozen_content.c redeclares MP_QSTR_port, ...); a .py
    file it no longer has stays frozen. A record from before module_revisions were kept can't say, so it counts
    as moved."""
    if had is None:
        return ["the build record doesn't name its modules' commits"]

    def at(row):
        return row["commit"][:12] if row.get("commit") else "(not a git checkout)"

    moved = []
    for name, row in now.items():
        old = had.get(name)
        if old is None:
            moved.append(f"{name}: not in the build record")
        elif old.get("commit") != row.get("commit"):
            moved.append(f"{name} was at {at(old)}, now {at(row)}")
    return moved


def foreign_sources(build, roots):
    """Objects a make build compiled from outside the module they belong to:
    (dependency file, source) for each, or [] when every one came from its
    own module.

    make ports name a module's sources <module>/<file>.c and find them through
    vpath, in the parent directories of the build's modules. When modules come
    from different places, a directory of the module's name beside another
    module's parent (an older checkout of it, say) can be searched first, and
    its copy is compiled, silently. Our py.mk patch searches each module's own
    directory first; this reads what the compiler was actually handed, from
    each object's dependency file (BUILD/<module>/<file>.P, whose first rule
    names the source first), so it catches the next way it goes wrong too.

    `roots` are the build's module directories (and anything else the build
    is meant to compile from, such as omnipython-lvgl's root): a source counts as its
    module's when the directory it was found under is inside one of them. A
    source inside the build directory was generated there and always counts.
    CMake ports name sources by absolute path and write no .P files."""
    roots = [Path(os.path.realpath(r)) for r in roots]
    here = Path(os.path.realpath(build))
    found = []
    for dep in sorted(Path(build).rglob("*.P")):
        rel = dep.relative_to(build).with_suffix("").as_posix()
        if "/" not in rel:
            continue  # the port's own files, built at the top of BUILD
        rule = []
        try:
            with open(dep, errors="replace") as f:
                for line in f:
                    rule.append(line.rstrip("\r\n").rstrip("\\"))
                    if not line.rstrip("\r\n").endswith("\\"):
                        break
        except OSError:
            continue
        _, colon, deps = " ".join(rule).partition(": ")
        if not colon or not deps.split():
            continue
        src = deps.split()[0].replace("\\", "/")
        stem = src.rpartition(".")[0]
        # Found through vpath as <module>/<file>, the source ends with the
        # object's path under BUILD. One relative to the port (../../py/gc.c)
        # or absolute all the way (BUILD/home/...) was never searched for.
        absolute = src.startswith("/") or re.match(r"[A-Za-z]:/", src)
        if not absolute or not stem.endswith("/" + rel) or stem == "/" + rel:
            continue
        # A source the build generated in its own directory belongs to it:
        # stm32's OpenAMP copies libmetal's sources into BUILD/openamp/.
        if here in Path(os.path.realpath(src)).parents:
            continue
        module = rel.split("/", 1)[0]
        found_in = Path(os.path.realpath(stem[: -len(rel)] + module))
        if not any(found_in == r or r in found_in.parents for r in roots):
            found.append((dep, src))
    return found


# ---- esp32: the generated board directory, the fragments, autosize --------

def esp32_chip(base):
    try:
        return json.loads((base / "board.json").read_text()).get("mcu", "")
    except (OSError, ValueError):
        return ""


FLASH_SIZES = ("2MB", "4MB", "8MB", "16MB", "32MB", "64MB", "128MB")


def esp32_fragment(chip, module_dirs, board, variant, flash, table, coredump=False):
    """Our sdkconfig, appended last. Each source is used only if it exists.
    coredump (--coredump) adds sdkconfig.coredump after every other source, so
    nothing before it can turn the flash core dump off."""
    parts = ["# Generated by build_mp.py. kconfgen takes the last assignment, so this", "# file is appended after the board's own.", ""]
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
    if coredump:
        sources += [VARIANTS_DIR / "esp32" / "sdkconfig.coredump"]
    used = []
    for src in sources:
        if src.is_file():
            used.append(src)
            parts += [f"# from {src}", src.read_text().rstrip(), ""]
    return "\n".join(parts) + "\n", used


def esp32_clocks(sdk):
    """The flash and PSRAM clocks an esp32 sdkconfig sets, in MHz (PSRAM None
    when the build has none)."""
    def mhz(pattern):
        m = re.search(pattern, sdk, re.M)
        return int(m.group(1)) if m else None
    return (mhz(r"^CONFIG_ESPTOOLPY_FLASHFREQ_(\d+)M=y$"),
            mhz(r"^CONFIG_SPIRAM_SPEED_(\d+)M=y$") if re.search(r"^CONFIG_SPIRAM=y$", sdk, re.M) else None)


# The highest CPU clock each chip is rated for, in MHz: the top of ESP-IDF's
# CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ choice for that target, which matches the
# datasheets. esp32_cpu_rated() covers the exceptions.
ESP32_CPU_RATED = {"esp32": 240, "esp32s2": 240, "esp32s3": 240, "esp32c2": 120, "esp32c3": 160,
                   "esp32c5": 240, "esp32c6": 160, "esp32c61": 160, "esp32h2": 96, "esp32p4": 400}


def esp32_cpu_rated(sdk):
    """The highest CPU clock, in MHz, for the chip an esp32 sdkconfig builds
    for, or None when the target isn't one we know.

    A P4 image for revisions before 3.0 is held to their 360 MHz (IDF offers
    400 there only as an experimental option). A single-core classic ESP32
    build is held to 160, the rating of the ESP32-S0WD and ESP32-D2WD it is
    usually for. A C5 with flash encryption is held to 160: IDF says encrypted
    flash writes aren't stable above it."""
    m = re.search(r'^CONFIG_IDF_TARGET="(\w+)"$', sdk, re.M)
    target = m.group(1) if m else ""
    rated = ESP32_CPU_RATED.get(target)

    def on(name):
        return re.search(rf"^CONFIG_{name}=y$", sdk, re.M) is not None

    if target == "esp32p4" and on("ESP32P4_SELECTS_REV_LESS_V3"):
        rated = 360
    elif target == "esp32" and on("FREERTOS_UNICORE"):
        rated = 160
    elif target == "esp32c5" and on("SECURE_FLASH_ENC_ENABLED"):
        rated = 160
    return rated


def esp32_cpu(sdk):
    """The CPU clock an esp32 sdkconfig sets and the chip's rated one, in MHz
    (either None when the sdkconfig doesn't say)."""
    m = re.search(r"^CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=(\d+)$", sdk, re.M)
    return (int(m.group(1)) if m else None), esp32_cpu_rated(sdk)


def esp32_speed_note(sdk, where="the board's variant sdkconfig"):
    """The CPU, flash and PSRAM should run at the fastest clock the chips are
    rated for. Below 80 MHz, code and data that miss the cache wait on a slow
    bus, and on a classic ESP32 that starves Bluetooth audio: two boards that
    fell behind in A2DP music at 40 MHz played clean at 80 MHz. A CPU below its
    rating runs out of time first: a 48 kHz synth with a reverb clicked on a
    classic ESP32 at 160 MHz and played clean at 240. Say so loudly after a
    build."""
    flash, psram = esp32_clocks(sdk)
    cpu, rated = esp32_cpu(sdk)
    # An image that also runs on a revision 0 P4 can't clock its flash above
    # 40 MHz: IDF offers no faster choice there.
    flash_floor = 40 if re.search(r"^CONFIG_ESP32P4_REV_MIN_0=y$", sdk, re.M) else 80
    slow = [f"the CPU at {cpu} MHz (rated for {rated})"] if cpu and rated and cpu < rated else []
    slow += [f"flash at {flash} MHz"] if flash and flash < flash_floor else []
    slow += [f"PSRAM at {psram} MHz"] if psram and psram < 80 else []
    if not slow:
        return
    runs = slow[0] if len(slow) == 1 else ", ".join(slow[:-1]) + " and " + slow[-1]
    say("\n" + "!" * 72)
    say(f"  This image runs {runs}.")
    say("  Run the CPU, flash and PSRAM at the fastest clock the board's chips are")
    say(f"  rated for, in {where}.")
    if cpu and rated and cpu < rated:
        say(f"  The CPU: set CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_{rated}=y,")
        say(f"  CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_{cpu}=n and CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ={rated},")
        say("  as variants/esp32/ESP32_GENERIC/AUDIO_KIT does. Below its rating, audio")
        say("  work runs out of time: a synth with a reverb clicks at 160 MHz on an ESP32.")
    if (flash and flash < flash_floor) or (psram and psram < 80):
        say("  Flash and PSRAM: most are rated for 80 MHz or more, and at 40 MHz a")
        say("  classic ESP32 can fall behind in Bluetooth audio. Check the chips'")
        say("  datasheets (esptool's flash-id reads the flash's JEDEC ID), then set")
        say("  CONFIG_ESPTOOLPY_FLASHFREQ_80M=y and CONFIG_SPIRAM_SPEED_80M=y, as")
        say("  variants/esp32/ESP32_GENERIC/QTPY_PICO does. The flash clock lives in")
        say("  the bootloader: flash it with the app.")
    say("!" * 72 + "\n")


def esp32_table(chip, board, variant):
    """Our partition table, by the same convention as the fragments, the most
    specific one found winning; None keeps the board's own."""
    found = [VARIANTS_DIR / "esp32" / board / variant / "partitions.csv"] if variant else []
    found += [VARIANTS_DIR / "esp32" / f"partitions.{chip}.csv", VARIANTS_DIR / "esp32" / "partitions.csv"]
    return next((t.resolve() for t in found if t.is_file()), None)


def has_coredump_partition(table):
    """Whether a partition table CSV has a coredump partition."""
    return any(re.match(r"\s*[^#,\s][^,]*,\s*data\s*,\s*coredump\s*,", line) for line in table.read_text().splitlines())


def link_board(gen, base, generated):
    """Link everything upstream's board holds into gen, except the files the
    caller generates there: ${MICROPY_BOARD_DIR} is an include directory and
    board cmake files name files in it."""
    for entry in sorted(base.iterdir()):
        if entry.name not in generated:
            (gen / entry.name).symlink_to(entry.resolve())


def esp32_board_dir(gen, base, board, variant, fragment_path, module_dirs, components=()):
    """Upstream's board, unchanged, plus our fragment, as BOARD_DIR= (the way
    cmods' build_mp.sh did it)."""
    if gen.exists():
        shutil.rmtree(gen)
    gen.mkdir(parents=True)
    vfile = f"mpconfigvariant_{variant}.cmake" if variant else "mpconfigvariant.cmake"
    link_board(gen, base, {"mpconfigboard.cmake", "mpconfigboard.h", vfile})
    lines = ["# Generated by build_mp.py: the board below, unchanged.", f"include({base.as_posix()}/mpconfigboard.cmake)"]
    for d in module_dirs:
        comps = d / "components"
        for c in sorted(comps.iterdir()) if comps.is_dir() else []:
            if (c / "CMakeLists.txt").exists():
                lines.append(f"list(APPEND EXTRA_COMPONENT_DIRS {c.resolve().as_posix()})")
    for c in components:
        lines.append(f"list(APPEND EXTRA_COMPONENT_DIRS {c.resolve().as_posix()})")
    (gen / "mpconfigboard.cmake").write_text("\n".join(lines) + "\n")
    # A variant of ours may carry an mpconfigboard.h of its own, included
    # before the board's. It is how a variant sets a C define that the port's
    # ESP-IDF components read as well: they include this header but never see
    # MICROPY_DEF_BOARD (TinyUSB's MICROPY_HW_USB_HS is one).
    ours = board_variant_dir("esp32", board, variant)
    header = ["// Generated by build_mp.py: the board's own header."]
    if ours and (ours / "mpconfigboard.h").exists():
        header.insert(1, f'#include "{(ours / "mpconfigboard.h").as_posix()}"')
    if (base / "mpconfigboard.h").exists():
        header.append(f'#include "{(base / "mpconfigboard.h").as_posix()}"')
    if len(header) > 1:
        (gen / "mpconfigboard.h").write_text("\n".join(header) + "\n")
    vlines = ["# Generated by build_mp.py. Our fragment comes LAST: kconfgen takes the", "# last assignment in SDKCONFIG_DEFAULTS."]
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


def rp2_board_dir(gen, base, board, variant):
    """Upstream's board, unchanged, plus what we add to it, as BOARD_DIR=.
    Every rp2 build gets one: its mpconfigboard.cmake includes upstream's,
    then variants/rp2/board.cmake, which runs the CPU at its chip's rated
    clock. A variant of ours adds its files too. The port takes BOARD_VARIANT=
    only when the board holds an mpconfigvariant_<V>.cmake, and builds the pin
    names from the board's pins.csv, so the generated directory holds ours: a
    variant file that includes upstream's default variant and then ours, our
    pins.csv when we have one (it replaces upstream's whole), and the board's
    header followed by ours (upstream's defines its name and hostname without
    #ifndef, so ours has to come after to #undef them). A variant of
    upstream's own is linked through with the rest of the board."""
    ours = board_variant_dir("rp2", board, variant) if variant else None
    vfile = f"mpconfigvariant_{variant}.cmake" if variant else None
    if variant and not ours and not (base / vfile).exists():
        die(f"{board} has no variant {variant}")
    if ours and (base / vfile).exists():
        die(f"{board} has a variant {variant} of its own")
    if gen.exists():
        shutil.rmtree(gen)
    gen.mkdir(parents=True)
    own = {n for n in ("mpconfigboard.h", "pins.csv") if ours and (ours / n).exists()}
    link_board(gen, base, {"mpconfigboard.cmake"} | ({vfile} if ours else set()) | own)
    (gen / "mpconfigboard.cmake").write_text(
        "# Generated by build_mp.py: the board's settings, then what every rp2 build of ours adds.\n"
        f"include({(base / 'mpconfigboard.cmake').as_posix()})\n"
        f"include({(VARIANTS_DIR / 'rp2' / 'board.cmake').resolve().as_posix()})\n")
    if not ours:
        return
    if "pins.csv" in own:
        (gen / "pins.csv").symlink_to((ours / "pins.csv").resolve())
    if "mpconfigboard.h" in own:
        (gen / "mpconfigboard.h").write_text(
            "// Generated by build_mp.py: the board's own header, then the variant's.\n"
            f'#include "{(base / "mpconfigboard.h").as_posix()}"\n'
            f'#include "{(ours / "mpconfigboard.h").as_posix()}"\n')
    (gen / vfile).write_text(
        "# Generated by build_mp.py: the board's default variant, then ours.\n"
        f"include({base.as_posix()}/mpconfigvariant.cmake OPTIONAL)\n"
        f"include({(ours / 'mpconfigvariant.cmake').as_posix()})\n")


# The highest CPU clock each rp2 chip is rated for, and the SDK's default, in
# MHz. Raspberry Pi certified the RP2040 for 200 MHz at a core voltage of at
# least 1.15 V (pico-sdk 2.1.1, which raises the voltage itself); the RP2350's
# datasheet rates it for 150.
RP2_CPU_RATED = {"rp2040": 200, "rp2350": 150}
RP2_CPU_DEFAULT = {"rp2040": 125, "rp2350": 150}


def rp2_cpu(cache, flags):
    """The CPU clock an rp2 build runs at and its chip's rating, in MHz, from
    its CMakeCache.txt and the firmware's compile flags (either None when they
    don't say). The clock is the SDK's SYS_CLK_HZ: given outright, the fastest
    supported one, or the chip's default."""
    m = re.search(r"^PICO_PLATFORM:\w+=(rp2040|rp2350)", cache, re.M)
    if not m:
        return None, None
    chip = m.group(1)
    for name, scale in (("HZ", 1_000_000), ("KHZ", 1000), ("MHZ", 1)):
        got = re.search(rf"-DSYS_CLK_{name}=\(?(\d+)", flags)
        if got:
            return int(got.group(1)) // scale, RP2_CPU_RATED[chip]
    if re.search(r"-DPICO_USE_FASTEST_SUPPORTED_CLOCK=1\b", flags):
        return RP2_CPU_RATED[chip], RP2_CPU_RATED[chip]
    return RP2_CPU_DEFAULT[chip], RP2_CPU_RATED[chip]


def rp2_build_flags(build):
    """The firmware's compile flags, from whichever generator CMake used."""
    for f in (build / "CMakeFiles" / "firmware.dir" / "flags.make", build / "build.ninja"):
        if f.exists():
            return f.read_text(errors="replace")
    return ""


def rp2_speed_note(cache, flags):
    """Say loudly after a build when its CPU runs below its chip's rating.
    variants/rp2/board.cmake sets the rated clock for every rp2 build, so
    this speaks up only when a board or variant set a slower one."""
    cpu, rated = rp2_cpu(cache, flags)
    if not cpu or not rated or cpu >= rated:
        return
    say("\n" + "!" * 72)
    say(f"  This image runs the CPU at {cpu} MHz; its chip is rated for {rated}.")
    say("  Every rp2 build sets the rated clock in variants/rp2/board.cmake")
    say("  (PICO_USE_FASTEST_SUPPORTED_CLOCK, which on an RP2040 also raises the")
    say("  core voltage to the 1.15 V that 200 MHz needs). Something in this build")
    say("  set SYS_CLK_HZ, SYS_CLK_KHZ or SYS_CLK_MHZ lower: take it out, unless")
    say("  the board can't run at its chip's rating.")
    say("!" * 72 + "\n")


def stm32_board_dir(gen, base, board, variant):
    """Upstream's board, unchanged, plus our variant's files, as BOARD_DIR=.
    As on rp2, the port takes BOARD_VARIANT= only when the board holds an
    mpconfigvariant_<V>.mk, so the generated directory holds one that
    includes ours. A header of ours named like one of the board's
    (mpconfigboard.h, stm32h7xx_hal_conf.h) comes after the board's, so it
    can add to it or #undef what it sets. Any other file of ours (a linker
    script fragment, say) is linked in beside the board's, where
    $(BOARD_DIR) finds it."""
    ours = board_variant_dir("stm32", board, variant)
    if not ours or not (ours / "mpconfigvariant.mk").exists():
        die(f"{board} has no variant {variant}")
    vfile = f"mpconfigvariant_{variant}.mk"
    if (base / vfile).exists():
        die(f"{board} has a variant {variant} of its own")
    extra = sorted(p for p in ours.iterdir() if p.name not in ("mpconfigvariant.mk", "manifest.py"))
    for p in extra:
        if (base / p.name).exists() and p.suffix != ".h":
            die(f"{p.relative_to(VARIANTS_DIR)} would replace the board's own {p.name}")
    if gen.exists():
        shutil.rmtree(gen)
    gen.mkdir(parents=True)
    link_board(gen, base, {vfile} | {p.name for p in extra})
    for p in extra:
        if (base / p.name).exists():
            (gen / p.name).write_text(
                "// Generated by build_mp.py: the board's own header, then the variant's.\n"
                f'#include "{(base / p.name).as_posix()}"\n'
                f'#include "{p.resolve().as_posix()}"\n')
        else:
            (gen / p.name).symlink_to(p.resolve())
    (gen / vfile).write_text(
        "# Generated by build_mp.py: the variant's settings, after the board's.\n"
        f"include {(ours / 'mpconfigvariant.mk').as_posix()}\n")


# A dual-core stm32 board whose M7 firmware may grow over the flash upstream
# leaves for an M4 program, the way an esp32 build grows its app partition.
# Its linker script names the M7's region FLASH_TEXT and the M4's FLASH_CM4.
STM32_M4_SLOT = {
    # board: (linker script, M7 region start, M4 region start, flash end)
    "ARDUINO_GIGA": ("stm32h747.ld", 0x08040000, 0x08180000, 0x08200000),
}
# Where an STM32H747 as shipped starts its M4 at reset (BOOT_CM4_ADD0, an
# option byte), whatever is there: the GIGA's sets BCM4, so it boots on its own.
STM32_M4_BOOT_DEFAULT = 0x08100000
M4_SLOT_MK = "pydevices-m4-slot.mk"


def stm32_plain_board_dir(gen, base):
    """Upstream's board as BOARD_DIR=, for a build with no variant of ours:
    the port includes $(BOARD_DIR)/mpconfigvariant.mk when it's there, which
    is where the M4 slot's settings go in."""
    if (base / "mpconfigvariant.mk").exists():
        die(f"{base.name} has an mpconfigvariant.mk of its own")
    if gen.exists():
        shutil.rmtree(gen)
    gen.mkdir(parents=True)
    link_board(gen, base, {"mpconfigvariant.mk"})
    (gen / "mpconfigvariant.mk").write_text("# Generated by build_mp.py: the board's settings, unchanged.\n")


def stm32_m4_slot(build, gen, base, board, variant):
    """Write the settings that move the M7 firmware's region over the M4's,
    and include them from the generated board's variant file. OpenAMP goes
    too: it loads and talks to an M4 program, and with the slot taken there
    is nowhere to put one (a remoteproc load would overwrite this firmware)."""
    ld_name, text, m4, end = STM32_M4_SLOT[board]
    ld = (base / ld_name).read_text()
    grown = re.sub(r"^(\s*FLASH_TEXT \(rx\)\s*:\s*ORIGIN = 0x[0-9A-Fa-f]+, LENGTH = )\d+K(.*)$",
                   lambda m: f"{m.group(1)}{(end - text) // 1024}K   /* CM7 firmware, over the CM4's slot too */",
                   ld, count=1, flags=re.M)
    grown, cm4 = re.subn(r"^\s*FLASH_CM4 \(rx\).*\n", "", grown, count=1, flags=re.M)
    if grown == ld or not cm4:
        die(f"{ld_name} doesn't lay out FLASH_TEXT and FLASH_CM4 as build_mp.py expects; the M4 slot can't be taken")
    out_ld = build / ld_name.replace(".ld", "_m4_slot.ld")
    out_ld.write_text(f"/* Generated by build_mp.py from boards/{board}/{ld_name}: the M7 firmware's\n"
                      f"   region runs on over the M4's ({hex(m4)}-{hex(end)}). */\n" + grown)
    (build / M4_SLOT_MK).write_text(
        "# Generated by build_mp.py: the firmware didn't fit its 1,280 KB region, so it\n"
        "# takes the M4 core's flash too, and leaves out OpenAMP, which runs M4 programs.\n"
        f"LD_FILES := $(patsubst boards/{board}/{ld_name},{out_ld.as_posix()},$(LD_FILES))\n"
        "MICROPY_PY_OPENAMP = 0\n"
        "MICROPY_PY_OPENAMP_REMOTEPROC = 0\n")
    vfile = gen / (f"mpconfigvariant_{variant}.mk" if variant else "mpconfigvariant.mk")
    vfile.write_text(vfile.read_text() + f"include {(build / M4_SLOT_MK).as_posix()}\n")


def stm32_overflow(log_text):
    """Bytes FLASH_TEXT overflowed by, from the linker's message, else None."""
    m = re.search(r"region `FLASH_TEXT' overflowed by (\d+) bytes", log_text)
    return int(m.group(1)) if m else None


def stm32_m4_boot_words(image, text):
    """The vector table the M4 would start from at STM32_M4_BOOT_DEFAULT if
    this image covers it: (stack pointer, reset, NMI, HardFault), else None."""
    off = STM32_M4_BOOT_DEFAULT - text
    data = Path(image).read_bytes()
    if len(data) < off + 16:
        return None
    return struct.unpack_from("<IIII", data, off)


# Memory that answers an M4 bus transaction: internal flash, SRAM1-3 (and
# their alias at 0x10000000), AXI SRAM, SRAM4, backup SRAM, both FMC windows
# (where the SDRAM sits depends on bank swapping) and the QSPI window. The
# M7's ITCM and DTCM aren't reachable from the M4.
M4_BUS_REGIONS = (
    (0x08000000, 0x08200000),
    (0x10000000, 0x10048000),
    (0x24000000, 0x24080000),
    (0x30000000, 0x30048000),
    (0x38000000, 0x38010000),
    (0x38800000, 0x38801000),
    (0x60000000, 0x80000000),
    (0x90000000, 0xA0000000),
    (0xC0000000, 0xE0000000),
)


def m4_reaches_memory(sp, pc, nmi, hf):
    """True when an M4 starting from this vector table would touch real
    memory: its reset address or HardFault handler (whatever the low bit)
    points at memory, or its stack does, where a fault's stacking writes.
    Anything else faults without a bus transaction and the M4 locks up."""
    return any(lo <= w < hi for w in ((sp - 4) & 0xFFFFFFFF, pc & ~1, hf & ~1) for lo, hi in M4_BUS_REGIONS)


#: Paddings tried step by this many bytes: a multiple of every alignment in
#: .text (8 at most on the GIGA), so a pad that grows by it moves all the code
#: after it on by just as much, and the bytes it brings to the M4's address can
#: be read off the image already linked.
M4_PAD_STEP = 16
#: The most padding tried. The windows it can bring to the M4's address lie 16
#: bytes apart over 4 KB, so no literal pool or table of addresses (a few dozen
#: bytes) can refuse them all, however the image grows around them.
M4_PAD_MAX = 4096
#: Relinks tried before giving up, when the bytes read off the image weren't
#: what the relink put there.
M4_PAD_TRIES = 4
M4_PAD_SYMBOL = "pydevices_m4_pad"


def stm32_m4_pad_module(build, pad):
    """A generated C module whose only code is a function with *pad* bytes of
    nops, kept by the linker and placed with the other modules, before the M4's
    address: growing it moves the code there. Returns its directory, for
    USER_C_MODULES; files are rewritten only when they change."""
    d = build / "m4pad-src" / "m4pad"
    d.mkdir(parents=True, exist_ok=True)
    mk = ("# Generated by build_mp.py: padding that moves the code at the M4's boot address.\n"
          f"SRC_USERMOD_C += $(USERMOD_DIR)/m4pad.c\nLDFLAGS_USERMOD += -Wl,-u,{M4_PAD_SYMBOL}\n")
    c = ("// Generated by build_mp.py: padding that moves the code at the M4's boot address.\n"
         f"void {M4_PAD_SYMBOL}(void);\n"
         f"void {M4_PAD_SYMBOL}(void) {{\n"
         f"    __asm volatile (\".rept {pad // 2}\\n nop\\n .endr\");\n}}\n")
    for name, text in (("micropython.mk", mk), ("m4pad.c", c)):
        f = d / name
        if not f.exists() or f.read_text() != text:
            f.write_text(text)
    return d


def stm32_m4_pad_now(build):
    """The padding the module in *build* holds now, in bytes."""
    c = build / "m4pad-src" / "m4pad" / "m4pad.c"
    m = re.search(r"\.rept (\d+)", c.read_text()) if c.exists() else None
    return int(m.group(1)) * 2 if m else 0


def elf_symbol(elf, name):
    """The value of symbol *name* in a 32-bit little-endian ELF file, or None."""
    data = Path(elf).read_bytes() if Path(elf).exists() else b""
    if data[:6] != b"\x7fELF\x01\x01":
        return None
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    want = name.encode() + b"\0"
    for _name, kind, _flags, _addr, off, size, link, _info, _align, entsize in sections:
        if kind != 2:  # SHT_SYMTAB
            continue
        strtab = sections[link][4]
        for at in range(off, off + size, entsize or 16):
            st_name, value = struct.unpack_from("<II", data, at)
            if data.startswith(want, strtab + st_name):
                return value
    return None


def stm32_m4_pad_candidates(image, text, pad, pad_at):
    """The paddings, smallest first, that would leave a vector table that
    doesn't reach memory at the M4's boot address, read off *image* (linked
    with *pad* bytes of padding, the padding function at address *pad_at*):
    with p bytes, the code that was p - pad bytes before that address is there.
    None when the padding lies after the address and can't move it."""
    boot = STM32_M4_BOOT_DEFAULT
    if pad_at is None or not text <= pad_at & ~1 < boot:
        return None
    pad_at &= ~1  # a Thumb function's address has its low bit set
    data = Path(image).read_bytes()
    out = []
    for p in range(0, M4_PAD_MAX + 1, M4_PAD_STEP):
        src = boot - text - (p - pad)
        # The window must come from code after the padding, wholly in the image.
        if src < pad_at - text + pad + 16 or src + 16 > len(data):
            continue
        if not m4_reaches_memory(*struct.unpack_from("<IIII", data, src)):
            out.append(p)
    return out


def stm32_m4_settle(image, text, pad, pad_at, relink):
    """Make sure *image* doesn't hand the M4 a vector table that reaches
    memory. Keeps the current *pad* when its table passes; otherwise picks the
    smallest padding whose table would pass, read off the image
    (stm32_m4_pad_candidates), and calls relink(pad) -> make's exit code, up to
    M4_PAD_TRIES times. *pad_at* () returns the padding function's address in
    the image linked now. Returns (pad, words, refused words or None), or
    (None, None, refused words) when relink failed."""
    words = stm32_m4_boot_words(image, text)
    if not (words and m4_reaches_memory(*words)):
        return pad, words, None
    first, tried = words, {pad}
    for _ in range(M4_PAD_TRIES):
        p = next((q for q in stm32_m4_pad_candidates(image, text, pad, pad_at()) or () if q not in tried), None)
        if p is None:
            break
        tried.add(p)
        if relink(p) != 0:
            return None, None, first
        pad, words = p, stm32_m4_boot_words(image, text)
        if not (words and m4_reaches_memory(*words)):
            return pad, words, first
    return pad, words, first


# TinyUSB, for the esp32 targets that build it. The port takes it from the
# IDF component manager, which fetches it into managed_components/ at
# configure time, so the overlay can't patch it. Instead a copy at the same
# tag, with patches/tinyusb/ applied, replaces it through the component
# manager's own override_path, declared by a one-file component of ours that
# the generated board directory adds to EXTRA_COMPONENT_DIRS. (A component of
# the same name in EXTRA_COMPONENT_DIRS isn't enough: the managed copy is
# still fetched, and ESP-IDF adds it later, so it wins.)
TINYUSB = "espressif__tinyusb"  # ESP-IDF names a component after its directory
TINYUSB_CHIPS = ("esp32s2", "esp32s3", "esp32p4")
TINYUSB_PATCHES = REPO / "patches" / "tinyusb"


def make_submodules(shell, make, mp):
    """A make port's `make submodules`, with patches/lib-tinyusb/ and
    patches/lib-btstack/ taken off their submodules before it and put back
    on after."""
    apply_patches.unprepare_submodules(mp)
    shell(make + ["submodules"])
    apply_patches.prepare_submodules(mp)


def tinyusb_component(mp, chip):
    """deps/espressif__tinyusb at its DEPENDENCIES.lock tag with patches/tinyusb/
    applied (one local commit, like the MicroPython tree's), or None for a
    chip without TinyUSB."""
    if chip not in TINYUSB_CHIPS:
        return None
    p = pin(TINYUSB)
    url, tag = p.url, p.tag
    # The copy replaces whatever TinyUSB the port asks for, so it must be the
    # one the prepared tree's manifest names (patch 0014).
    manifest = (mp / "ports" / "esp32" / "main" / "idf_component.yml").read_text()
    block = re.search(r"^  espressif/tinyusb:\n((?:    .*\n|\s*\n)+)", manifest, re.M)
    git_src = re.search(r"^\s+git:\s*(\S+)", block.group(1), re.M) if block else None
    version = re.search(r"^\s+version:\s*(\S+)", block.group(1), re.M) if block else None
    if not (git_src and version) or (git_src.group(1), version.group(1)) != (url, tag):
        die(f"the port builds TinyUSB {git_src.group(1) if git_src else '?'} {version.group(1) if version else '?'}, "
            f"but {pins.LOCK} patches {url} {tag}: move them together")
    dest = DEPS / TINYUSB
    if not dest.exists():
        DEPS.mkdir(exist_ok=True)
        clone_at(url, tag, dest, commit=p.ref)
    patches = sorted(TINYUSB_PATCHES.glob("*.patch"))
    digest = hashlib.sha256()
    for patch in patches:
        digest.update(patch.read_bytes())
    sid = "Patch-Series: " + digest.hexdigest()[:16]
    mark = f"PyDevices patches applied to {tag} (a local record, never pushed)"
    head = git_out(dest, "log", "-1", "--format=%s%n%b").splitlines()
    clean = not git_out(dest, "status", "--porcelain")
    if patches and head[:1] == [mark] and sid in head and clean:
        return dest
    if not patches and git_out(dest, "describe", "--tags", "--exact-match") == tag and clean:
        return dest
    run(["git", "-C", str(dest), "-c", "advice.detachedHead=false", "checkout", "-q", "--force", tag])
    run(["git", "-C", str(dest), "clean", "-fdq"])
    for patch in patches:
        if subprocess.run(["git", "-C", str(dest), "apply", str(patch)]).returncode != 0:
            die(f"{patch.relative_to(REPO)} does not apply to TinyUSB {tag}")
        say(f"applied {patch.relative_to(REPO)}")
    if patches:
        run(["git", "-C", str(dest), "add", "-A"])
        run(["git", "-C", str(dest), "-c", "user.name=pydevices", "-c", "user.email=pydevices@local",
             "commit", "-q", "-m", mark, "-m", sid])
    return dest


def tinyusb_override(shim, tinyusb):
    """The component that points the port's espressif/tinyusb at `tinyusb`."""
    if shim.exists():
        shutil.rmtree(shim)
    shim.mkdir(parents=True)
    (shim / "CMakeLists.txt").write_text("# Generated by build_mp.py: no sources, only the manifest beside it.\nidf_component_register()\n")
    (shim / "idf_component.yml").write_text(
        "# Generated by build_mp.py: the port's TinyUSB, with patches/tinyusb/ applied.\n"
        "dependencies:\n"
        "  espressif/tinyusb:\n"
        f'    override_path: "{tinyusb.as_posix()}"\n')
    return shim


def autosize_headroom(flash_bytes):
    """Room left above an image that autosize grew to fit, so a few KB of growth
    doesn't move the filesystem again: 1/32 of the flash, 64 KB to 256 KB."""
    return max(0x10000, min(0x40000, (flash_bytes // 32) & ~0xFFFF))


# The smallest filesystem autosize leaves on an esp32: room for a board's
# settings, its drivers and an app or two. Growing the app past this fails the
# build instead of flashing a board with no filesystem.
ESP32_MIN_VFS = 0x20000


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
    fitted = (image + align - 1) & ~(align - 1)
    size = (fitted + headroom + align - 1) & ~(align - 1)
    # The filesystem is the last data partition, or the flash after the last
    # partition. On a small flash the headroom alone can take all of it, so it
    # shrinks first, down to none, to leave ESP32_MIN_VFS.
    flash_bytes = int(flash.group(1)) * 1024 * 1024 if flash else 0
    if flash_bytes:
        named = rows[-1] if len(rows) > idx + 1 and rows[-1][1] == "data" and rows[-1][2] in ("fat", "littlefs", "spiffs") else None
        between = sum(num(r[4]) for r in rows[idx + 1:] if r is not named)
        room = flash_bytes - num(rows[idx][3]) - between
        if room - fitted < ESP32_MIN_VFS:
            die(f"autosize: an image of {hex(image)} needs an app partition of {hex(fitted)}, which leaves "
                f"{max(0, room - fitted) // 1024} KB for the filesystem in {flash.group(1)} MB, under the "
                f"{ESP32_MIN_VFS // 1024} KB autosize keeps. Build fewer modules, or use a board with more flash.")
        size = min(size, room - ESP32_MIN_VFS)
    rows[idx][4] = hex(size)
    cursor = num(rows[idx][3]) + size
    for r in rows[idx + 1:]:
        r[3] = hex(cursor)
        cursor += num(r[4])
    after = rows[idx + 1:]
    moved = (", ".join(f"'{r[0]}'" for r in after) + " moved to follow it. ") if after else ""
    # A table that names its filesystem gives it a size, and growing the app
    # pushes that partition past the end of the flash. The filesystem is the
    # part that can shrink: cut the last data partition to what is left.
    if after and flash_bytes and cursor > flash_bytes:
        last = after[-1]
        if last[1] == "data" and last[2] in ("fat", "littlefs", "spiffs"):
            room = flash_bytes - num(last[3])
            if room < 0x10000:
                die(f"autosize: the app grew to {hex(size)} and leaves no room for '{last[0]}' in {flash.group(1)} MB")
            last[4] = hex(room)
            cursor = flash_bytes
            moved += f"'{last[0]}' cut to {hex(room)} to fit the flash. "
    if any(r[1] == "data" and r[2] in ("fat", "littlefs", "spiffs") for r in after):
        fs = ["# The filesystem's partition moved with them."]
    else:
        fs = ["# The filesystem isn't listed: it is the rest of the flash after the last",
              f"# partition here, so it now starts at {hex(cursor)} and is smaller."]
    body = ["# Name, Type, SubType, Offset, Size, Flags",
            f"# Generated by build_mp.py from {src}: '{rows[idx][0]}' grown to {hex(size)} for an",
            f"# image of {hex(image)}. {moved}".rstrip()] + fs + [""]
    body += [", ".join(r).rstrip(", ") for r in rows]
    dest.write_text("\n".join(body) + "\n")
    return dest


def fs_size(text):
    """--keep-fs's size in bytes, from "589824", "576K" or "576KB"; None if
    it isn't one, or isn't whole 4 KB flash sectors."""
    m = re.fullmatch(r"(\d+)\s*([KM]?)B?", text.strip().upper())
    if not m:
        return None
    n = int(m.group(1)) * {"": 1, "K": 1024, "M": 1024 * 1024}[m.group(2)]
    return n if n and n % 4096 == 0 else None


def keep_fs_refused(keep_fs, fits):
    """What a build with --keep-fs says when the firmware doesn't fit beside it."""
    say(f"\nThe firmware doesn't fit beside a {keep_fs // 1024} KB filesystem."
        + (f" A {fits // 1024} KB one would" if fits else ""))
    say(("  fit it, but that" if fits else "  A smaller one") + " moves the filesystem and a board flashed with it comes up")
    say("  with an empty one. (--keep-fs, so nothing was rebuilt: build fewer modules,")
    say("  or copy the board's files off and build without --keep-fs.)")


# A filesystem partition, by its subtype. MicroPython mounts the one named vfs
# (or ffat); with neither, it makes one of the flash after the last partition.
ESP32_FS_SUBTYPES = ("fat", "littlefs", "spiffs")


def esp32_table_rows(src):
    rows = []
    for raw in Path(src).read_text().splitlines():
        line = raw.strip()
        if line and not line.startswith("#"):
            fields = [f.strip() for f in line.split(",")]
            rows.append((fields + [""] * 6)[:6])
    return rows


def esp32_bytes(v):
    v = v.strip().lower()
    mult = 1024 if v.endswith("k") else 1024 * 1024 if v.endswith("m") else 1
    return int(v.rstrip("km"), 0) * mult


def esp32_used_table(build, port_dir):
    """The partition table a configured build in `build` used, or None."""
    try:
        sdk = (build / "sdkconfig").read_text()
    except OSError:
        return None
    used = re.search(r'^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="(.*)"$', sdk, re.M)
    if not used:
        return None
    src = Path(used.group(1))
    return src if src.is_absolute() else port_dir / src


def esp32_keep_fs_layout(rows, flash_bytes, fs_bytes):
    """(app row, named filesystem row or None, app size) for a table whose
    filesystem keeps fs_bytes at the end of the flash. The app partition takes
    what the partitions after it leave."""
    apps = [i for i, r in enumerate(rows) if r[1] == "app"]
    if len(apps) != 1:
        die(f"--keep-fs sizes a table with one app partition; this one has {len(apps)}")
    idx = apps[0]
    fs = [i for i, r in enumerate(rows) if r[1] == "data" and r[2] in ESP32_FS_SUBTYPES]
    if fs and (fs != [len(rows) - 1] or fs[0] < idx):
        die("--keep-fs needs the filesystem partition, if the table names one, last and after the app")
    named = fs[0] if fs else None
    between = sum(esp32_bytes(r[4]) for i, r in enumerate(rows) if i > idx and i != named)
    return idx, named, flash_bytes - fs_bytes - esp32_bytes(rows[idx][3]) - between


def esp32_keep_fs(src, flash_bytes, fs_bytes, dest):
    """The table at src with its app partition sized so the filesystem is the
    last fs_bytes of the flash, where a board flashed earlier with the same
    size has it."""
    rows = esp32_table_rows(src)
    idx, named, size = esp32_keep_fs_layout(rows, flash_bytes, fs_bytes)
    if size < 0x10000:
        die(f"--keep-fs: a {fs_bytes // 1024} KB filesystem leaves no room for the app in {flash_bytes // 1024 // 1024} MB")
    rows[idx][4] = hex(size)
    cursor = esp32_bytes(rows[idx][3]) + size
    for i in range(idx + 1, len(rows)):
        rows[i][3] = hex(cursor)
        if i != named:
            cursor += esp32_bytes(rows[i][4])
    if named is not None:
        rows[named][4] = hex(fs_bytes)
    where = "" if named is not None else " (the flash after the last partition)"
    body = ["# Name, Type, SubType, Offset, Size, Flags",
            f"# Generated by build_mp.py from {src} for --keep-fs: '{rows[idx][0]}' sized to {hex(size)}",
            f"# so the filesystem{where} is {fs_bytes // 1024} KB at {hex(flash_bytes - fs_bytes)}."]
    body += [", ".join(r).rstrip(", ") for r in rows]
    dest.write_text("\n".join(body) + "\n")
    return dest


def esp32_keep_fs_fits(log_text, table, flash_bytes):
    """The largest filesystem, in whole 64 KB, that leaves room for an app the
    size the log says it is: 0 if none does, None if the log isn't an app
    partition overflow."""
    m = re.search(r"app partition is too small for binary \S+ size (0x[0-9a-fA-F]+)", log_text)
    if not m:
        return None
    _, _, room = esp32_keep_fs_layout(esp32_table_rows(table), flash_bytes, 0)
    room -= (int(m.group(1), 16) + 0xFFFF) & ~0xFFFF
    return room & ~0xFFFF if room >= 0x10000 else 0


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
# ESP-IDF components a module needs on CircuitPython's espressif port that
# ESP-IDF doesn't ship. MicroPython's esp32 build gets them from Espressif's
# component manager; CircuitPython builds with it off, so they are pinned in
# DEPENDENCIES.lock, fetched into deps/, and handed to make as
# USER_ESP_IDF_COMPONENT_DIRS (one of our CircuitPython patches).
CP_ESP_IDF_COMPONENTS = {"cameraif": ("esp_cam_sensor", "esp_sccb_intf", "cmake_utilities"),
                         "h264enc": ("esp_h264",),
                         "usbhost": ("usb",)}
CP_ESP_IDF_COMPONENT_DEPS = {n for names in CP_ESP_IDF_COMPONENTS.values() for n in names}


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
    """deps/circuitpython at its DEPENDENCIES.lock tag, and a venv with its build tools.

    Always a checkout of our own, never a link to a sibling circuitpython/: a
    sibling is at whatever version its owner needs, and may carry other
    repositories' patches. Only the submodules a target needs are fetched,
    by CircuitPython's own tools/ci_fetch_deps.py, at build time."""
    p = pin("circuitpython")
    url, version = p.url, p.tag
    if not CP.exists():
        DEPS.mkdir(exist_ok=True)
        clone_at(url, version, CP, commit=p.ref)
    have = dep_version("circuitpython", CP)
    if have != version:
        die(f"deps/circuitpython is {have or 'not at a tag'}, but {pins.LOCK} wants {version}")
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
    if lvgl_root_of(real):
        return LVGL_REPO
    top = git_out(real, "rev-parse", "--show-toplevel")
    common = git_out(real, "rev-parse", "--path-format=absolute", "--git-common-dir")
    if top and Path(top).resolve() == real and Path(common).name == ".git":
        return Path(common).parent.name
    return real.name


# MicroPython's own upstream packages (aioble, usb-device, ...): CircuitPython
# has its own, so a CircuitPython build never takes this module's manifest.
MP_ONLY_MODULES = {"micropython-lib"}


def cp_frozen_manifest_dir(d):
    """Whether this module's manifest.py freezes Python into a CircuitPython
    build: it has one, it names no c_module() (those reach CircuitPython
    through USER_C_MODULES), and it isn't one of MicroPython's own."""
    real = d.resolve()
    manifest = real / "manifest.py"
    return (manifest.is_file() and module_name(d) not in MP_ONLY_MODULES
            and "c_module(" not in manifest.read_text())


def cp_board_frozen_dirs(cp, port, board):
    """The directories a CircuitPython board freezes with FROZEN_MPY_DIRS in
    its mpconfigboard.mk. Setting FROZEN_MANIFEST on the make line beats the
    board's own generated manifest, so a build with a manifest of ours freezes
    these itself."""
    mk = cp / "ports" / port / "boards" / board / "mpconfigboard.mk"
    dirs = []
    for line in mk.read_text().splitlines() if mk.is_file() else []:
        m = re.match(r"\s*FROZEN_MPY_DIRS\s*\+?=\s*([^#]*)", line)
        for word in (m.group(1).split() if m else []):
            word = word.replace("$(TOP)", str(cp))
            if "$(" in word:
                die(f"{mk} freezes {word}: build_mp.py reads only $(TOP) paths in FROZEN_MPY_DIRS")
            dirs.append(word)
    return dirs


# CircuitPython's asyncio and the clock it runs on. A build that freezes our
# Python freezes these too, because bledev, appkit and the rest `import asyncio`
# and no CircuitPython board freezes one of its own by default. The pair come
# from CircuitPython's frozen/ submodules, the way a board's FROZEN_MPY_DIRS
# would name them.
CP_FROZEN_LIBRARIES = ("Adafruit_CircuitPython_asyncio", "Adafruit_CircuitPython_Ticks")


def cp_frozen_library_dirs(cp, board_dirs):
    """CP_FROZEN_LIBRARIES' directories, less any the board already freezes."""
    have = {Path(d).name for d in board_dirs}
    return [str(cp / "frozen" / n) for n in CP_FROZEN_LIBRARIES if n not in have]


def ensure_cp_frozen_libraries(cp):
    """Check out CP_FROZEN_LIBRARIES (shallow, with tags: their __version__ is
    made from `git describe`), as ci_fetch_deps.py does for the boards that name them,
    and the semver tool the preprocessing imports, which it fetches only later."""
    if not any((cp / "tools" / "python-semver").glob("semver*")):
        run(["git", "submodule", "update", "--init", "--depth", "1", "tools/python-semver"], cwd=cp,
            stdout=subprocess.DEVNULL)
    for name in CP_FROZEN_LIBRARIES:
        sub = f"frozen/{name}"
        if not any((cp / sub).glob("*.py")) and not (cp / sub / "asyncio").is_dir():
            run(["git", "submodule", "update", "--init", "--depth", "1", sub], cwd=cp, stdout=subprocess.DEVNULL)
        if not git_out(cp / sub, "describe", "--tags", "--exact-match"):
            run(["git", "fetch", "--tags", "--depth", "1"], cwd=cp / sub, stdout=subprocess.DEVNULL)


def cp_frozen_libraries_record(cp, board_dirs):
    """name and version of each library cp_frozen_manifest froze, for the build record."""
    out = []
    for d in cp_frozen_library_dirs(cp, board_dirs):
        version = git_out(d, "describe", "--tags", "--exact-match") or git_out(d, "describe", "--tags", "--always")
        out.append(f"{Path(d).name} {version}")
    return out


def cp_frozen_manifest(cp, port, board, variant, module_dirs, library_packages, build, library):
    """The FROZEN_MANIFEST of a CircuitPython build that freezes our Python, or
    None when it freezes none: the port's own base (the unix coverage variant's
    manifest), the board's FROZEN_MPY_DIRS content, the Python of the modules
    named (palettes, pdwidgets, omnipython-lvgl's fs_driver, ...), and the
    library packages named. Returns (manifest path, make arguments)."""
    manifests = [d for d in module_dirs if cp_frozen_manifest_dir(d)]
    if not manifests and not library_packages:
        return None, []
    lines = ["# Generated by build_mp.py: what this CircuitPython build freezes."]
    extra = []
    base = cp / "ports" / port / "variants" / (variant or "coverage") / "manifest.py"
    if not board and base.is_file():
        lines.append(f'include("{base.as_posix()}")')
    board_dirs = cp_board_frozen_dirs(cp, port, board) if board else []
    frozen_dirs = board_dirs + cp_frozen_library_dirs(cp, board_dirs)
    if frozen_dirs:
        ensure_cp_frozen_libraries(cp)
        # What circuitpy_mpconfig.mk does with FROZEN_MPY_DIRS when it makes the
        # manifest itself.
        out = build / "frozen_mpy_board"
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        run([sys.executable, str(cp / "tools" / "preprocess_frozen_modules.py"), "-o", str(out), *frozen_dirs],
            env=dict(os.environ, PYTHONPATH=str(cp / "tools" / "python-semver")), stdout=subprocess.DEVNULL)
        for f in sorted(out.rglob("*.py")):
            lines.append(f'freeze_as_mpy("{out.as_posix()}", "{f.relative_to(out).as_posix()}")')
    if board_dirs:
        extra.append("FROZEN_MPY_DIRS=")
    if library_packages or any(LIBRARY in (d / "manifest.py").read_text() for d in manifests):
        lines.append(f'include("{(MODULES_DIR / "library.py").as_posix()}")')
    for d in manifests:
        lines.append(f'include("{d.resolve().as_posix()}")')
    for name in library_packages:
        lines.append(f'require("{name}", library="{LIBRARY}", opt=3)')
    manifest = build / "frozen-manifest.py"
    manifest.write_text("\n".join(lines) + "\n")
    return manifest, [f"FROZEN_MANIFEST={manifest.as_posix()}", *extra]


def cp_user_c_modules(module_dirs, link_dir):
    """The directory holding each module's micropython.mk, for USER_C_MODULES.

    CircuitPython's py/py.mk carries MicroPython 1.29's USER_C_MODULES block,
    so the micropython.mk our MicroPython builds use is the one it reads.
    A module without C is Python only: cp_frozen_manifest() freezes its
    manifest.py.

    Each is given to make through a link named for the module, in link_dir:
    py.mk puts a module's objects under BUILD/<its directory's name>/, so two
    worktrees both called cp-phase1 would build into one place."""
    found_dirs = []
    for d in module_dirs:
        real = d.resolve()
        found = next((c for c in (real, real / "code") if (c / "micropython.mk").is_file()), None)
        if not found:
            if cp_frozen_manifest_dir(d):
                continue  # Python only: frozen through cp_frozen_manifest()
            die(f"module '{d.name}' has no micropython.mk and no manifest.py that freezes Python for CircuitPython: "
                "it builds through CMake, for MicroPython's esp32 port, and CircuitPython's make ports can't take it yet "
                "(docs/build-plan.md lists which modules build there)")
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


def build_dir_for(build, want, clean, revisions):
    """The build dir for a target, emptied first if it was built with
    something other than `want` (another module set, flash size, ...) or
    with its modules at other commits than `revisions`, record()'s rows."""
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
        elif moved := modules_moved(had.get("module_revisions"), revisions):
            shutil.rmtree(build)
            say(f"{build} was built from modules that have moved since; wiped:\n" + "\n".join("  " + m for m in moved))
    elif build.exists() and any(build.iterdir()):
        # Something built here without a record: nobody can say with what.
        shutil.rmtree(build)
        say(f"{build} has no build record; wiped")
    build.mkdir(parents=True, exist_ok=True)
    # Written before the build, so a build that fails still says what it was
    # built with, and the next one with a different set wipes it.
    rec_path.write_text(json.dumps(dict(want, complete=False, module_revisions=revisions), indent=1) + "\n")
    return rec_path


# The unix port freezes pdwidgets and palettes beside pydevices and the C
# modules, and their names together pass the 64 KB a ROM qstr pool's two-byte
# offsets can address. Four-byte offsets (patches/circuitpython/0001 and 0010)
# lift that for about two bytes of flash per qstr: fine on a desktop, so only
# the unix port asks. A board's cell is the same as before.
CP_UNIX_CFLAGS = "-DMICROPY_QSTR_OFFSET_BYTES=4"


def cp_make_args(port, make_extra):
    """make's arguments with what a CircuitPython port needs from us added.
    A CFLAGS_EXTRA of the caller's is kept, with ours appended to it."""
    if port != "unix":
        return list(make_extra)
    out, found = [], False
    for a in make_extra:
        if a.startswith("CFLAGS_EXTRA="):
            a, found = f"{a} {CP_UNIX_CFLAGS}", True
        out.append(a)
    return out if found else out + [f"CFLAGS_EXTRA={CP_UNIX_CFLAGS}"]


def circuitpython_version(cp, patches):
    """What CircuitPython's own build would print for the commit `cp` is at: its
    `git describe` of the tag, such as 11.0.0-alpha.1, or None without tags.

    Our patch series is laid on the checkout as uncommitted edits, so
    CircuitPython's py/version.py sees them and appends -dirty. The build
    records which patches it carries (circuitpython_patches), so they are not
    what "dirty" means here; edits beyond them still are. That is checked by
    applying the series to a scratch index and asking git whether the files
    match it. The result goes to the build as CP_VERSION, which py/version.py
    takes in place of asking git."""
    described = git_out(cp, "describe", "--tags", "--first-parent", "--match", "[!v]*", "HEAD")
    if not described:
        say("CircuitPython has no tags here; its version will show a commit, not a version")
        return None
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(tmp) / "index"))
        git = ["git", "-C", str(cp)]
        ok = subprocess.run(git + ["read-tree", "HEAD"], env=env, capture_output=True).returncode == 0
        for patch in patches:
            ok = ok and subprocess.run(git + ["apply", "--cached", str(patch)], env=env,
                                       capture_output=True).returncode == 0
        # --ignore-submodules: ci_fetch_deps moves submodules, which no patch of ours touches.
        edited = not ok or subprocess.run(git + ["diff", "--quiet", "--ignore-submodules=all"], env=env,
                                          capture_output=True).returncode != 0
    return described + ("-dirty" if edited else "")


def build_circuitpython(args, make_extra, ws):
    if os.name == "nt":
        die("--interpreter circuitpython builds from Linux or WSL")
    if args.flash:
        die("--flash is for MicroPython's esp32 port")
    # Before anything is fetched.
    refuse_old_lvgl_names([Path(i.strip()).name for i in (args.modules or "").split(",")])
    ensure_modules(ws)
    cp = ensure_circuitpython()
    lock = lock_beside(cp)  # noqa: F841 -- held until exit
    cp_patches = prepare_circuitpython(cp)
    version = circuitpython_version(cp, sorted(CP_PATCHES.glob("*.patch")))

    port = args.port or choose("CircuitPython port:", cp_ports())
    if port not in cp_ports():
        die(f"no CircuitPython port '{port}' (have: {', '.join(cp_ports())})")
    make_extra = cp_make_args(port, make_extra)
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
    library = Library(ws)
    spec, module_dirs, library_packages = resolve_modules(spec, library, "circuitpython", lambda: lvgl_checkout(ws))
    if library_packages or any(LIBRARY in (d.resolve() / "manifest.py").read_text()
                               for d in module_dirs if cp_frozen_manifest_dir(d)):
        library.build()

    # The unix port's own default variant is coverage.
    build = OUT_DIR / "circuitpython" / port / (board or variant or "coverage")
    module_set = sorted([d.name if d.parent == MODULES_DIR else str(d) for d in module_dirs] + library_packages)
    components = []
    if port == "espressif":
        for d in module_dirs:
            for name in CP_ESP_IDF_COMPONENTS.get(module_name(d), ()):
                if name not in components:
                    components.append(name)
    component_dirs = [ensure_dep(name, ws) for name in components]
    # make's own arguments are part of what was built: a CIRCUITPY_X=0 left
    # out of the next build would otherwise keep its generated module table.
    want = {"interpreter": "circuitpython", "port": port, "board": board, "variant": variant,
            "modules": spec, "module_set": module_set, "make_args": make_extra}
    if components:
        want["esp_idf_components"] = [f"{n} {dep_version(n, d)}" for n, d in zip(components, component_dirs)]
    revisions = record(module_dirs, library.source)
    rec_path = build_dir_for(build, want, args.clean, revisions)
    user_c = cp_user_c_modules(module_dirs, build / "usermods")
    frozen, frozen_args = (None, []) if any(a.startswith("FROZEN_MANIFEST=") for a in make_extra) else \
        cp_frozen_manifest(cp, port, board, variant, module_dirs, library_packages, build, library)

    env = {k: v for k, v in os.environ.items()
           if k not in ("USER_C_MODULES", "FROZEN_MANIFEST", "BUILD", "BOARD", "VARIANT", "VARIANT_DIR", "PYTHONPATH",
                        "BINDINGS_DIR", "PYDEVICES_LVGL")}
    if library.tree:
        env["PYDEVICES_LIBRARY"] = library.tree.as_posix()
    env["PYDEVICES_INTERPRETER"] = "circuitpython"
    if version:
        # py/version.py takes this instead of asking git, whose answer carries -dirty for our patches.
        env["CP_VERSION"] = version
    # The unix port is a desktop: pydevices freezes its host-only files there.
    env["PYDEVICES_PLATFORM"] = "desktop" if port == "unix" else "board"
    env["PATH"] = os.pathsep.join([str(CP_VENV / "bin"), env.get("PATH", "")])
    env["VIRTUAL_ENV"] = str(CP_VENV)
    # Only the submodules this target needs, the way CircuitPython's CI does:
    # a board name fetches its port's, "tests" what the unix port builds with.
    run([str(CP_VENV / "bin" / "python"), "tools/ci_fetch_deps.py", board or "tests"], cwd=cp, env=env,
        stdout=subprocess.DEVNULL)
    # A board that freezes libraries (FROZEN_MPY_DIRS) builds its manifest with
    # micropython-lib's tools, which ci_fetch_deps leaves out of a board's list.
    if board and not (cp / "lib" / "micropython-lib" / "README.md").exists():
        run(["git", "submodule", "update", "--init", "--depth", "1", "lib/micropython-lib"], cwd=cp,
            stdout=subprocess.DEVNULL)
    # mpy-cross first and on its own: a port's sub-make for it would inherit
    # BUILD= from our command line (the same trap as micropython#19667).
    run(["make", "-C", str(cp / "mpy-cross"), "-j", JOBS], env=env, stdout=subprocess.DEVNULL)

    # make is given a short relative BUILD, a link in the port dir to ours: the
    # raspberrypi link rule echoes every object path in one shell argument,
    # and with our absolute build path in front of each that passes Linux's
    # 128 KB limit on a single argument ("Argument list too long").
    #
    # The link's name is also where a coverage binary writes its profile data
    # when it runs, so a build under another OUT_DIR gets a link of its own:
    # two binaries sharing one would each find the other's .gcda files and
    # print "overwriting an existing profile data" on every exit.
    port_dir = cp / "ports" / port
    tag = board or variant or "coverage"
    if OUT_DIR != (REPO / "builds").resolve():
        tag += "-" + hashlib.sha1(str(build).encode()).hexdigest()[:8]
    short = port_dir / f"build-{tag}"
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
    if component_dirs:
        make.append("USER_ESP_IDF_COMPONENT_DIRS=" + " ".join(str(d) for d in component_dirs))
    make += frozen_args + make_extra
    say(f"CircuitPython {dep_version('circuitpython', cp)} {port} {board or variant or 'coverage'}; "
        f"USER_C_MODULES: {' '.join(str(d) for d in user_c) or '(none)'}"
        + (f"; frozen: {frozen}" if frozen else ""))
    prefix = cp_espressif_env(port_dir, board) if port == "espressif" else ""
    rc = subprocess.run([BASH, "-c", prefix + shlex.join(make)], env=env).returncode
    if rc != 0:
        die(f"the build failed (make exit {rc})")
    sdk = build / "esp-idf" / "sdkconfig"
    if port == "espressif" and sdk.exists():
        esp32_speed_note(sdk.read_text(), "the board's sdkconfig (CIRCUITPY_ESP_FLASH_FREQ and\n  CIRCUITPY_ESP_PSRAM_FREQ in its mpconfigboard.mk for flash and PSRAM)")
    rec = dict(want, complete=True)
    rec["circuitpython"] = git_out(cp, "describe", "--tags", "--always", "--dirty")
    rec["circuitpython_patches"] = cp_patches
    if version:
        rec["circuitpython_version"] = version
    if frozen:
        rec["frozen_libraries"] = cp_frozen_libraries_record(cp, cp_board_frozen_dirs(cp, port, board) if board else [])
    rec["module_revisions"] = revisions
    rec_path.write_text(json.dumps(rec, indent=1) + "\n")
    say(f"\nBuilt into {build}")
    for name in ("firmware.uf2", "firmware.bin", "firmware.elf", "micropython"):
        if (build / name).exists():
            say(f"  {build / name}")


# ---- the build -------------------------------------------------------------

def failure_hint(port, log_text):
    """One line naming the fix for a build failure we recognise, or None."""
    if port == "windows" and os.name != "nt" and "LT_SYS_SYMBOL_USCORE" in log_text:
        # libffi's autoreconf, without libltdl's m4 macros.
        return "libffi's autoreconf can't find libltdl's macros: install libltdl-dev (Debian, Ubuntu), then build again"
    return None


EXAMPLES = """examples:
  ./build_mp.py                       asks for port, board, variant and modules
  ./build_mp.py --port unix --variant pydevices --modules all
  ./build_mp.py --port windows --variant pydevices --modules all
  ./build_mp.py --port esp32 --board ESP32_GENERIC_S3 --variant SPIRAM_OCT \\
      --flash 8MB --modules all
  ./build_mp.py --port rp2 --board RPI_PICO2 --modules displayif,pygraphics

Output lands in builds/<port>/[<board>/]<variant>/. Arguments it doesn't
recognise go to make (-j4, V=1). The guide: docs/newcomers.md."""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], allow_abbrev=False,
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=EXAMPLES)
    ap.add_argument("--interpreter", choices=("micropython", "circuitpython"), default="micropython",
                    help="circuitpython: CircuitPython-compatible firmware, from CircuitPython's own ports and boards")
    ap.add_argument("--port")
    ap.add_argument("--board")
    ap.add_argument("--variant")
    ap.add_argument("--modules", help='comma list: short names, full paths, or "all"')
    ap.add_argument("--flash", help="esp32 flash size, e.g. 16MB")
    ap.add_argument("--no-autosize", action="store_true", help="refuse instead of growing the firmware's flash: esp32's app partition, rp2's firmware over its "
                    "filesystem, the Arduino GIGA's over its M4 core's flash")
    ap.add_argument("--keep-fs", metavar="SIZE", help="rp2 and esp32: build beside the filesystem a board already has, its size in bytes or KB "
                    "(576K), and refuse instead of moving it when the firmware doesn't fit")
    ap.add_argument("--coredump", action="store_true",
                    help="esp32: a troubleshooting build that writes a core dump to flash on a crash (standard builds don't)")
    ap.add_argument("--clean", action="store_true", help="delete this target's build dir first")
    ap.add_argument("--python-to-romfs", action="store_true",
                    help="Arduino GIGA: freeze no Python; compile it into romfs/ beside the firmware instead, "
                         "for the board's ROMFS partition, which leaves internal flash to the C modules")
    args, make_extra = ap.parse_known_args()
    if args.coredump and args.interpreter != "micropython":
        die("--coredump is for MicroPython on esp32")

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
    upstream = pin(pins.MICROPYTHON).ref
    apply_patches.prepare(mp, upstream, apply_patches.series(), refresh=True)
    version = micropython_version(mp, upstream)

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
    if args.coredump and port != "esp32":
        die("--coredump is for esp32")
    if args.keep_fs and port not in ("rp2", "esp32"):
        die("--keep-fs is for rp2 and esp32")
    keep_fs = fs_size(args.keep_fs) if args.keep_fs else None
    if args.keep_fs and not keep_fs:
        die(f"--keep-fs takes a size in whole 4 KB sectors, in bytes or KB (576K), not '{args.keep_fs}'")
    if args.flash and args.flash.upper() not in FLASH_SIZES:
        die(f"--flash takes one of {', '.join(FLASH_SIZES)}")
    flash = args.flash.upper() if args.flash else None
    if keep_fs and port == "esp32" and not flash:
        die("--keep-fs on esp32 needs --flash: the filesystem is the end of the flash, so its place depends on the flash size")
    if port == "esp32" and flash is None and not args.port:
        # Upstream's generic tables assume 4 MB, which most module sets outgrow.
        flash = choose("Flash size (none keeps the board's own):", list(FLASH_SIZES), allow_none=True)

    spec = args.modules
    if spec is None:
        if not sys.stdin.isatty():
            die("--modules: not given, and there is no terminal to ask on")
        say("Modules: " + ", ".join(module_choices()))
        spec = input('Which (comma list; full paths for others; "all"; empty for none)? ').strip()
    library = Library(ws)
    spec, module_dirs, library_packages = resolve_modules(spec, library, "micropython", lambda: lvgl_checkout(ws))
    lvgl_root = lvgl_in(module_dirs)
    if library_packages or uses_library(module_dirs, frozen_manifest(port, board, variant)):
        library.build()

    # A troubleshooting build has its own folder, so it never replaces the
    # standard image beside it.
    build = OUT_DIR / port / (board or "") / ((variant or DEFAULT_VARIANT.get(port, "default")) + ("-coredump" if args.coredump else ""))
    build = Path(os.path.normpath(build))
    rec_path = build / "pydevices-build.json"
    # The set --modules resolved to, not just its spelling: "all" grows when a
    # module is added, and a build dir from before would otherwise be reused,
    # its frozen content generated against a qstr pool the new module has
    # since changed (frozen_content.c then redeclares MP_QSTR_Image, ...).
    module_set = sorted([d.name if d.parent == MODULES_DIR else str(d) for d in module_dirs] + library_packages)
    want = {"port": port, "board": board, "variant": variant, "flash": flash, "modules": spec,
            "module_set": module_set}
    if args.python_to_romfs:
        want["python_to_romfs"] = True
    # Each module's commit too: a module that moved keeps its name, and make
    # would reuse frozen content and qstr headers from before it moved.
    revisions = record(module_dirs, library.source)
    build_dir_for(build, want, args.clean, revisions)

    env = dict(os.environ)
    for leak in ("USER_C_MODULES", "FROZEN_MANIFEST", "BUILD", "BOARD", "VARIANT", "BOARD_DIR", "VARIANT_DIR",
                 "BINDINGS_DIR", "PYDEVICES_LVGL", "PYDEVICES_BOARD_MANIFEST"):
        env.pop(leak, None)
    env["PYDEVICES_MODULES"] = spec
    own = board_manifest(port, board)
    if own is not None:
        env["PYDEVICES_BOARD_MANIFEST"] = own.as_posix()
    if port == "stm32" and board in STM32_M4_SLOT:
        # The padding module that moves the code at the M4's boot address
        # when it must; empty until then. Regenerated before each make.
        env["PYDEVICES_MODULES"] = ",".join(x for x in (spec, (build / "m4pad-src" / "m4pad").as_posix()) if x)
    romfs_list = build / "romfs-python.json"
    if args.python_to_romfs:
        if port != "stm32" or board not in STM32_M4_SLOT:
            die("--python-to-romfs is for the Arduino GIGA, whose manifest moves its Python to ROMFS")
        # The board's manifest writes its Python modules here and freezes none.
        romfs_list.unlink(missing_ok=True)
        env["PYDEVICES_PYTHON_TO_ROMFS"] = romfs_list.as_posix()
    # The frozen pydevices leaves its host-only files off a board's image.
    env["PYDEVICES_PLATFORM"] = "desktop" if port in ("unix", "windows", "webassembly") else "board"
    if version:
        # makeversionhdr.py takes these instead of asking git, which a shallow clone can't answer.
        env["MICROPY_GIT_TAG"], env["MICROPY_GIT_HASH"] = version
    if library.tree:
        env["PYDEVICES_LIBRARY"] = library.tree.as_posix()
    if lvgl_root:
        # modules/all/manifest.py takes LVGL's port from here.
        env["PYDEVICES_LVGL"] = lvgl_port(lvgl_root, "micropython").as_posix()
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
    make.append(f"FROZEN_MANIFEST={frozen_manifest(port, board, variant).as_posix()}")
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

    # make keeps an object whose source it now finds somewhere else, if the
    # object is newer, so one compiled from a stray copy by a build made before
    # py.mk searched each module's own directory first would stay in. Drop
    # those, and this build compiles them again from their modules.
    # The variant tree is ours too: a variant's manifest adds its own C module
    # (webassembly's wasmbridge) from beside the variant.
    roots = module_dirs + ([lvgl_root] if lvgl_root else []) + [VARIANTS_DIR / port]
    for dep, src in foreign_sources(build, roots):
        say(f"compiling {dep.relative_to(build).with_suffix('.o').as_posix()} again: it was compiled from {src}")
        dep.with_suffix(".o").unlink(missing_ok=True)
        dep.unlink()

    rc, out = 0, ""
    tinyusb = None
    stm32_grown = False
    stm32_m4_pad = 0
    try:
        if port == "esp32":
            base = board_dir(port, board)
            chip = esp32_chip(base)
            gen = build / board
            frag = build / "sdkconfig.pydevices"
            table = esp32_table(chip, board, variant)
            if args.coredump and not (table and has_coredump_partition(table)):
                die("--coredump: this board's table has no coredump partition yet, and the dump needs one. "
                    "The ESP32-S3 boards' table has it.")
            tinyusb = tinyusb_component(mp, chip)
            flash_bytes = int(flash[:-2]) * 1024 * 1024 if flash else 0
            kept = build / "partitions-keep-fs.csv"
            if keep_fs and table:
                table = esp32_keep_fs(table, flash_bytes, keep_fs, kept)
            for attempt in (1, 2):
                text, used = esp32_fragment(chip, module_dirs, board, variant, flash, table, args.coredump)
                frag.write_text(text)
                shims = [tinyusb_override(build / "tinyusb_override", tinyusb)] if tinyusb else []
                esp32_board_dir(gen, base, board, variant, frag, module_dirs, shims)
                make_esp = [c for c in make if not c.startswith("BOARD=")] + [f"BOARD={board}", f"BOARD_DIR={gen}"]
                # A saved sdkconfig beats SDKCONFIG_DEFAULTS; ours is generated.
                (build / "sdkconfig").unlink(missing_ok=True)
                if attempt == 1:
                    say(f"esp32 {board}{' ' + variant if variant else ''} ({chip}); our sdkconfig: {frag}")
                    for u in used:
                        say(f"  fragment: {u}")
                    shell(make_esp + ["submodules"])
                rc, out = shell(make_esp, log=True)
                if keep_fs and table != kept:
                    # The board's own table, found only once a build has
                    # configured: size it, and build against that.
                    src = esp32_used_table(build, port_dir)
                    if not src and rc:
                        break
                    if not src:
                        die("--keep-fs: this build didn't say which partition table it used, so there is none to size")
                    table = esp32_keep_fs(src, flash_bytes, keep_fs, kept)
                    say(f"\n--keep-fs: building once more against {table}, sized from the board's {src.name}.\n")
                    continue
                if keep_fs:
                    fits = esp32_keep_fs_fits(out, table, flash_bytes) if rc else None
                    if fits is not None:
                        keep_fs_refused(keep_fs, fits)
                    break
                if rc == 0 or attempt == 2:
                    break
                grown = esp32_autosize(out, build, port_dir, build / "partitions.csv")
                if not grown:
                    break
                if args.no_autosize:
                    say(f"\nThe app doesn't fit its partition. A table that fits: {grown}\n(--no-autosize, so nothing was rebuilt.)")
                    break
                say(f"\nautosize: the app didn't fit; building once more against {grown}.")
                say("  Growing the app partition moves the filesystem, which is the flash after it:")
                say("  a board flashed with this image comes up with an empty one. Copy your files")
                say("  back after flashing, or build with --no-autosize to stop here instead.\n")
                table = grown
            if rc == 0:
                esp32_speed_note((build / "sdkconfig").read_text())
            if rc == 0 and table:
                sdk = (build / "sdkconfig").read_text()
                got = re.search(r'^CONFIG_PARTITION_TABLE_FILENAME="(.*)"$', sdk, re.M)
                if not got or Path(got.group(1)) != table:
                    die(f"the build used {got.group(1) if got else 'no'} partition table, not {table}")
                rows = [l for l in table.read_text().splitlines() if l and not l.startswith("#")]
                fs = "" if re.search(r",\s*data\s*,\s*(fat|littlefs|spiffs)\b", "\n".join(rows)) else \
                    " (the filesystem is the flash after the last partition)"
                say(f"partition layout in this image{fs}:\n" + "\n".join("  " + l for l in rows))
                if args.coredump:
                    say("This is a troubleshooting build: a crash writes a core dump to the 'coredump' partition.")
        elif port == "rp2":
            gen = build / board
            rp2_board_dir(gen, board_dir(port, board), board, variant)
            make += [f"BOARD_DIR={gen}"]
            # CMake runs only when the build has no Makefile yet, so a build
            # configured against another board directory (upstream's, before
            # every build had one of ours) would keep it: configure again.
            cache = build / "CMakeCache.txt"
            got = re.search(r"^MICROPY_BOARD_DIR:\w+=(.*)$", cache.read_text(), re.M) if cache.exists() else None
            if got and Path(got.group(1)) != gen:
                (build / "Makefile").unlink(missing_ok=True)
            make_submodules(shell, make, mp)
            storage = keep_fs
            if keep_fs:
                got = re.search(r"^MICROPY_HW_FLASH_STORAGE_BYTES:\w+=(\d+)$", cache.read_text(), re.M) if cache.exists() else None
                if not got or int(got.group(1)) != keep_fs:
                    # The port runs CMake only when the build has no Makefile yet.
                    (build / "Makefile").unlink(missing_ok=True)
            for attempt in (1, 2):
                extra = [f"MICROPY_HW_FLASH_STORAGE_BYTES={storage}"] if storage else []
                rc, out = shell(make + extra, log=True)
                if rc == 0 or attempt == 2:
                    break
                storage = rp2_autosize(out)
                if not storage:
                    break
                if keep_fs:
                    keep_fs_refused(keep_fs, storage)
                    break
                if args.no_autosize:
                    say(f"\nThe firmware doesn't fit. MICROPY_HW_FLASH_STORAGE_BYTES={storage} would fit it.")
                    say("(--no-autosize, so nothing was rebuilt.)")
                    break
                say(f"\nautosize: the firmware didn't fit; building once more with a {storage // 1024} KB filesystem.")
                say("  Shrinking the filesystem moves it: a board flashed with this image comes up")
                say("  with an empty one. Copy your files back after flashing, or build with")
                say("  --no-autosize to stop here instead. To keep a board's filesystem where it")
                say("  is, build with --keep-fs and its size: on the board, import rp2, then")
                say("  f = rp2.Flash(); print(f.ioctl(4, 0) * f.ioctl(5, 0)).\n")
                # The port runs CMake only when the build has no Makefile yet.
                (build / "Makefile").unlink(missing_ok=True)
            # What the image was built with, grown now or on an earlier run (CMake caches it).
            got = re.search(r"^MICROPY_HW_FLASH_STORAGE_BYTES:\w+=(\d+)$", cache.read_text(), re.M) if cache.exists() else None
            rp2_storage = int(got.group(1)) if rc == 0 and got else None
            if rc == 0 and cache.exists():
                rp2_speed_note(cache.read_text(), rp2_build_flags(build))
        elif port == "stm32" and board in STM32_M4_SLOT:
            gen = build / board
            make += [f"BOARD_DIR={gen}"]
            # An earlier run here took the M4's slot: start there, not from
            # a build that is known not to fit.
            grown = (build / M4_SLOT_MK).exists()
            if grown and args.no_autosize:
                for entry in build.iterdir():
                    if entry.name != "pydevices-build.json":
                        shutil.rmtree(entry) if entry.is_dir() and not entry.is_symlink() else entry.unlink()
                say(f"--no-autosize: {build} had taken the M4 core's flash; wiped, building in 1,280 KB.")
                grown = False
            for attempt in (1, 2):
                stm32_m4_pad_module(build, stm32_m4_pad_now(build))
                if variant:
                    stm32_board_dir(gen, board_dir(port, board), board, variant)
                else:
                    stm32_plain_board_dir(gen, board_dir(port, board))
                if grown:
                    stm32_m4_slot(build, gen, board_dir(port, board), board, variant)
                if attempt == 1:
                    make_submodules(shell, make, mp)
                rc, out = shell(make, log=True)
                if rc == 0 or attempt == 2 or grown:
                    break
                over = stm32_overflow(out)
                if over is None:
                    break
                _, text, m4, end = STM32_M4_SLOT[board]
                if args.no_autosize:
                    say(f"\nThe firmware is {over:,} bytes over its {(m4 - text) // 1024:,} KB region. Taking the M4 core's")
                    say(f"flash too ({(end - text) // 1024:,} KB in all) would fit it. (--no-autosize, so nothing was rebuilt.)")
                    break
                say(f"\nautosize: the firmware is {over:,} bytes over its {(m4 - text) // 1024:,} KB region, so it takes the")
                say(f"  M4 core's flash too (0x{m4:08X}-0x{end - 1:08X}, {(end - text) // 1024:,} KB in all) and is built")
                say("  again from scratch. That image leaves out OpenAMP: the board can't run an M4")
                say("  program beside it. Build with --no-autosize to stop here instead.\n")
                # OpenAMP's settings change what every object was compiled with.
                for entry in build.iterdir():
                    if entry.name != "pydevices-build.json":
                        shutil.rmtree(entry) if entry.is_dir() and not entry.is_symlink() else entry.unlink()
                grown = True
            stm32_grown = grown and rc == 0
            if rc == 0:
                image = build / "firmware0.bin" if (build / "firmware0.bin").exists() else build / "firmware.bin"
                pad_before = stm32_m4_pad_now(build)

                def relink(p):
                    stm32_m4_pad_module(build, p)
                    # Dropping the padding leaves every object older than the
                    # firmware, and make would keep the padded link.
                    (build / "firmware.elf").unlink(missing_ok=True)
                    return shell(make, log=True)[0]

                pad, words, refused = stm32_m4_settle(image, STM32_M4_SLOT[board][1], pad_before,
                                                      lambda: elf_symbol(build / "firmware.elf", M4_PAD_SYMBOL), relink)
                if pad is None:
                    rc = 1
                elif words and m4_reaches_memory(*words):
                    die(f"{image.name} hands the M4 core a vector table that reaches memory (stack {hex(words[0])}, "
                        f"reset {hex(words[1])}, HardFault {hex(words[3])}) at {hex(STM32_M4_BOOT_DEFAULT)}, where "
                        "the M4 of a board as shipped starts at reset on its own (BOOT_CM4_ADD0, with BCM4 set), "
                        f"and no padding up to {M4_PAD_MAX:,} bytes before it moved it clear. Look at what "
                        f"{build / 'firmware.map'} puts there, or turn BCM4 off on your board: the flash guide says how.")
                else:
                    stm32_m4_pad = pad
                    if refused:
                        say(f"m4 pad: {pad} bytes of padding added before 0x{STM32_M4_BOOT_DEFAULT:08X}: without it "
                            f"the M4's vector table there (stack {hex(refused[0])}, reset {hex(refused[1])}, "
                            f"HardFault {hex(refused[3])}) reached memory, and an M4 starting from it can hang the board.")
        else:
            if port == "stm32" and board_variant_dir(port, board, variant):
                gen = build / board
                stm32_board_dir(gen, board_dir(port, board), board, variant)
                make += [f"BOARD_DIR={gen}"]
            make_submodules(shell, make, mp)
            if port == "windows" and os.name != "nt":  # cross-built: failure_hint() reads the log
                rc, out = shell(make, log=True)
            else:
                rc = shell(make)
    finally:
        if port == "esp32":
            # The component manager rewrites these on every build; the tree's own copy is the record.
            subprocess.run(["git", "-C", str(mp), "checkout", "--quiet", "--", "ports/esp32/lockfiles"], check=False)

    if rc != 0:
        hint = failure_hint(port, out)
        die(f"the build failed (make exit {rc})" + (f"\n{hint}" if hint else ""))
    stray = [f"{dep.relative_to(build).with_suffix('.o').as_posix()} was compiled from {src}"
             for dep, src in foreign_sources(build, roots)]
    if stray:
        shown = "\n  ".join(stray[:10]) + (f"\n  ... and {len(stray) - 10} more" if len(stray) > 10 else "")
        die(f"some of this build's objects were compiled from outside their module:\n  {shown}\n"
            "A directory of the module's name beside another module was searched before the module's own.")
    if args.python_to_romfs:
        python_to_romfs(romfs_list, build / "romfs", Path(env["MICROPY_MPYCROSS"]), mpy_cross_arch(mp, port, board))
    rec = dict(want, complete=True)
    if port == "rp2" and rp2_storage:
        rec["rp2_storage_bytes"] = rp2_storage
    if port == "stm32" and board in STM32_M4_SLOT:
        _, text, m4, end = STM32_M4_SLOT[board]
        rec["stm32_firmware_region_kb"] = ((end if stm32_grown else m4) - text) // 1024
        rec["stm32_m4_slot_taken"] = stm32_grown
        rec["stm32_m4_pad_bytes"] = stm32_m4_pad
    rec["micropython"] = git_out(mp, "describe", "--always", "--abbrev=12")
    if version:
        rec["micropython_version"] = version[0]
    rec["module_revisions"] = revisions
    rec["micropython_lib"] = apply_patches.lib_record(mp)
    if port == "esp32" and tinyusb:
        rec["tinyusb"] = git_out(tinyusb, "describe", "--tags", "--always")
    rec_path.write_text(json.dumps(rec, indent=1) + "\n")
    say(f"\nBuilt into {build}")
    for name in ("firmware.bin", "firmware0.bin", "firmware1.bin", "firmware.dfu", "firmware.uf2",
                 "micropython", "micropython.exe", "micropython.mjs", "micropython.wasm"):
        if (build / name).exists():
            say(f"  {build / name}")
    if args.python_to_romfs:
        say(f"  {build / 'romfs'}/  (the Python modules: add them to the board's ROMFS image, at its root)")


# The -march mpy-cross takes for an STM32 series, as ports/stm32/Makefile sets it.
STM32_MPY_ARCH = {"f0": "armv6m", "g0": "armv6m", "l0": "armv6m", "f4": "armv7emsp", "l4": "armv7emsp",
                  "wb": "armv7emsp", "g4": "armv7emsp", "f7": "armv7emdp", "h7": "armv7emdp"}


def mpy_cross_arch(mp, port, board):
    """The -march for a board's .mpy files, so native and viper code in them runs."""
    if port != "stm32":
        die("--python-to-romfs is for the Arduino GIGA (stm32)")
    text = (mp / "ports" / "stm32" / "boards" / board / "mpconfigboard.mk").read_text()
    series = re.search(r"^MCU_SERIES\s*=\s*(\w+)", text, re.MULTILINE)
    if not series or series.group(1) not in STM32_MPY_ARCH:
        die(f"--python-to-romfs: no mpy-cross architecture known for {board}")
    return STM32_MPY_ARCH[series.group(1)]


def python_to_romfs(listing, out, mpy_cross, arch):
    """Compile the Python modules a --python-to-romfs build left out of its
    frozen content into out/, laid out as they import, for the board's ROMFS."""
    if not listing.is_file():
        die(f"--python-to-romfs: the build wrote no {listing.name}; this board's manifest doesn't move its Python")
    shutil.rmtree(out, ignore_errors=True)
    files = json.loads(listing.read_text())
    for source, target, opt in files:
        dest = out / target
        dest.parent.mkdir(parents=True, exist_ok=True)
        if target.endswith(".py"):
            cmd = [str(mpy_cross), f"-march={arch}", "-s", target, "-o", str(dest.with_suffix(".mpy")), source]
            if opt is not None:
                cmd.insert(1, f"-O{opt}")
            run(cmd)
        else:
            shutil.copyfile(source, dest)
    size = sum(f.stat().st_size for f in out.rglob("*") if f.is_file())
    say(f"python-to-romfs: {len(files)} modules, {size:,} bytes, in {out}")


if __name__ == "__main__":
    main()
