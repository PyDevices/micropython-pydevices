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
import hashlib
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
        # UPSTREAM is a tag or a full commit: clone --branch takes only the first.
        clone_at("https://github.com/micropython/micropython", (REPO / "UPSTREAM").read_text().strip(), MP)
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


# lvgl-bindings is pinned in modules.lock beside the modules, but it is not a
# module: it is the generated binding and LVGL that lvgl-micropython compiles.
# It lands in modules/.lvgl-bindings (the dot keeps it out of "all" and of the
# module list) and reaches lvgl-micropython as BINDINGS_DIR, so that module's
# own pin and fetch never come into a build made here.
LVGL_BINDINGS = "lvgl-bindings"
LVGL_MODULE = "lvgl-micropython"


def ensure_modules(ws):
    for name, url, commit in read_lock("modules.lock"):
        if name in (LVGL_BINDINGS, LIBRARY):
            continue
        dest = MODULES_DIR / name
        if dest.exists() or dest.is_symlink():
            continue
        if ws and (ws / name).is_dir():
            link(dest, ws / name)
        else:
            clone_at(url, commit, dest)


def lvgl_bindings(ws, module_dirs):
    """The lvgl-bindings a build with lvgl-micropython compiles, or None
    when the build has no lvgl-micropython.

    In a workspace, modules/.lvgl-bindings links to the sibling checkout and
    builds it as it is, like every other module. Anywhere else, it is fetched
    at its modules.lock commit: lvgl-bindings' generated/ and lv_conf.h, and
    LVGL's src/ at the commit lvgl-bindings records. Shallow, sparse and
    blobless, so it downloads tens of MB rather than LVGL's whole history."""
    if not any(module_name(d) == LVGL_MODULE for d in module_dirs):
        return None
    rows = {row[0]: row[1:] for row in read_lock("modules.lock")}
    if LVGL_BINDINGS not in rows:
        die(f"modules.lock has no {LVGL_BINDINGS} row, which {LVGL_MODULE} needs")
    url, commit = rows[LVGL_BINDINGS]
    dest = MODULES_DIR / f".{LVGL_BINDINGS}"
    if dest.is_symlink():
        if (dest / "lv_conf.h").is_file():
            return dest
        dest.unlink()
    elif dest.is_dir():
        if git_out(dest, "rev-parse", "HEAD") == commit and (dest / "lvgl" / "lvgl.h").is_file():
            return dest
        say(f"{dest} is not at modules.lock's {commit[:12]}; fetching it again")
        shutil.rmtree(dest)
    sibling = ws / LVGL_BINDINGS if ws else None
    if sibling and (sibling / "lv_conf.h").is_file():
        if not (sibling / "lvgl" / "lvgl.h").is_file():
            die(f"{sibling} has no LVGL: git -C {sibling} submodule update --init --depth 1 lvgl")
        link(dest, sibling)
        return dest
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        die(f"modules.lock pins {LVGL_BINDINGS} at {commit!r}: it takes a full 40-character commit")
    say(f"fetching {url} at {commit[:12]} into {dest} (generated/, lv_conf.h and LVGL's src/)")
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
                    f" is modules.lock's {LVGL_BINDINGS} commit on GitHub?")

    sparse(part, url, commit, ["generated", "lvgl"])
    lvgl_sha = git_out(part, "ls-tree", "HEAD", "lvgl").split()[2:3]
    lvgl_url = git_out(part, "config", "-f", ".gitmodules", "submodule.lvgl.url")
    if not lvgl_sha or not lvgl_url:
        shutil.rmtree(part, ignore_errors=True)
        die(f"{LVGL_BINDINGS} {commit[:12]} records no lvgl submodule")
    shutil.rmtree(part / "lvgl")
    sparse(part / "lvgl", lvgl_url, lvgl_sha[0], ["src"])
    part.rename(dest)
    return dest


# omnipython-lib is the repository of our Python-only packages: pydevices,
# palettes, pdwidgets, appkit, audioinstruments, audioeffects, and every board
# config and driver. It is pinned in modules.lock beside the modules but is not
# one: a build asks its tools/libbuild.py for the library tree, one directory
# per package with a manifest.py beside its sources, and registers that tree
# with add_library(). A manifest then require()s a package from it. Packages
# with C stay modules.
#
# The checkout lands in modules/.omnipython-lib (the dot keeps it out of
# "all"): in the workspace a link to the sibling checkout, which wins like
# every other module, anywhere else a clone at the pinned commit. The tree is
# assembled into deps/omnipython-lib on every build that needs it; the outside
# packages the library pins by tag (pygraphics, audiocomponents' two) are
# cloned once into deps/omnipython-lib-sources.
LIBRARY = "omnipython-lib"


def library_checkout(ws):
    """modules/.omnipython-lib: the library repository, at its modules.lock commit
    (or the workspace's own checkout of it)."""
    rows = {row[0]: row[1:] for row in read_lock("modules.lock")}
    if LIBRARY not in rows:
        die(f"modules.lock has no {LIBRARY} row, which the library packages come from")
    url, commit = rows[LIBRARY]
    dest = MODULES_DIR / f".{LIBRARY}"
    if dest.is_symlink():
        if (dest / "tools" / "libbuild.py").is_file():
            return dest
        dest.unlink()
    elif dest.is_dir():
        if git_out(dest, "rev-parse", "HEAD") == commit:
            return dest
        say(f"{dest} is not at modules.lock's {commit[:12]}; fetching it again")
        shutil.rmtree(dest)
    # Called pydevices until the rename; either name is the same repository.
    for sibling in ((ws / LIBRARY, ws / "pydevices") if ws else ()):
        if (sibling / "tools" / "libbuild.py").is_file():
            link(dest, sibling)
            return dest
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        die(f"modules.lock pins {LIBRARY} at {commit!r}: it takes a full 40-character commit")
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
            r = subprocess.run(
                [sys.executable, str(self.source / "tools" / "libbuild.py"), "build", "--tree-only",
                 "--root", str(self.source), "--out", str(out), "--cache", str(DEPS / f"{LIBRARY}-sources"),
                 "--library-name", LIBRARY], text=True, capture_output=True)
            if r.returncode != 0:
                die(f"{self.source / 'tools' / 'libbuild.py'} failed:\n{r.stdout}{r.stderr}")
            self.tree = out / "library"
        return self.tree

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


def frozen_manifest(port, board, variant):
    """The manifest a build freezes. A variant of ours, of a port or of a
    board, may carry a manifest.py that includes modules/manifest.py and adds
    what only that variant freezes; every other build freezes
    modules/manifest.py itself."""
    mine = board_variant_dir(port, board, variant) if board else our_variant_dir(port, variant)
    if mine and (mine / "manifest.py").exists():
        return mine / "manifest.py"
    return MODULES_DIR / "manifest.py"


def module_choices():
    return sorted(p.name for p in MODULES_DIR.iterdir() if p.is_dir() and not p.name.startswith("."))


def resolve_modules(spec, library=None):
    """The comma list as modules/manifest.py will read it (paths made
    absolute), the module directories it amounts to ("all" expanded), which
    the esp32 conventions and the build record need, and the library packages
    it names. A name that is no module of ours but a package of the library is
    required from it; `library` is the Library to ask, built only then."""
    names, dirs, packages = [], [], []
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
        elif (MODULES_DIR / item).is_dir():
            names.append(item)
            dirs.append(MODULES_DIR / item)
        elif library and item in library.names():
            names.append(item)
            packages.append(item)
        else:
            have = module_choices() + (sorted(library.names()) if library else [])
            die(f"no module '{item}' in {MODULES_DIR} (have: {', '.join(have)})")
    unique = []
    for d in dirs:
        if d.resolve() not in [u.resolve() for u in unique]:
            unique.append(d)
    return ",".join(names), unique, sorted(set(packages))


def record(dirs, bindings=None, library=None):
    """A row per module, one for the lvgl-bindings lvgl-micropython compiles
    when the build has it, and one for the omnipython-lib the build's library
    packages came from: which commit went in, and whether it had uncommitted
    changes."""
    rows = {}
    named = ([(module_name(d), d) for d in dirs] + ([(LVGL_BINDINGS, bindings)] if bindings else [])
             + ([(LIBRARY, library)] if library else []))
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
    return rows


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
    is meant to compile from, such as lvgl-bindings): a source counts as its
    module's when the directory it was found under is inside one of them.
    CMake ports name sources by absolute path and write no .P files."""
    roots = [Path(os.path.realpath(r)) for r in roots]
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


def esp32_table(chip, board, variant):
    """Our partition table, by the same convention as the fragments, the most
    specific one found winning; None keeps the board's own."""
    found = [VARIANTS_DIR / "esp32" / board / variant / "partitions.csv"] if variant else []
    found += [VARIANTS_DIR / "esp32" / f"partitions.{chip}.csv", VARIANTS_DIR / "esp32" / "partitions.csv"]
    return next((t.resolve() for t in found if t.is_file()), None)


def has_coredump_partition(table):
    """Whether a partition table CSV has a coredump partition."""
    return any(re.match(r"\s*[^#,\s][^,]*,\s*data\s*,\s*coredump\s*,", line) for line in table.read_text().splitlines())


def esp32_board_dir(gen, base, board, variant, fragment_path, module_dirs, components=()):
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


def tinyusb_component(mp, chip):
    """deps/espressif__tinyusb at its deps.lock tag with patches/tinyusb/
    applied (one local commit, like the MicroPython tree's), or None for a
    chip without TinyUSB."""
    if chip not in TINYUSB_CHIPS:
        return None
    url, tag = {row[0]: row[1:] for row in read_lock("deps.lock")}[TINYUSB]
    # The copy replaces whatever TinyUSB the port asks for, so it must be the
    # one the prepared tree's manifest names (patch 0014).
    manifest = (mp / "ports" / "esp32" / "main" / "idf_component.yml").read_text()
    block = re.search(r"^  espressif/tinyusb:\n((?:    .*\n|\s*\n)+)", manifest, re.M)
    git_src = re.search(r"^\s+git:\s*(\S+)", block.group(1), re.M) if block else None
    version = re.search(r"^\s+version:\s*(\S+)", block.group(1), re.M) if block else None
    if not (git_src and version) or (git_src.group(1), version.group(1)) != (url, tag):
        die(f"the port builds TinyUSB {git_src.group(1) if git_src else '?'} {version.group(1) if version else '?'}, "
            f"but deps.lock patches {url} {tag}: move them together")
    dest = DEPS / TINYUSB
    if not dest.exists():
        DEPS.mkdir(exist_ok=True)
        clone_at(url, tag, dest)
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
    after = rows[idx + 1:]
    moved = (", ".join(f"'{r[0]}'" for r in after) + " moved to follow it. ") if after else ""
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
# deps.lock, fetched into deps/, and handed to make as
# USER_ESP_IDF_COMPONENT_DIRS (one of our CircuitPython patches).
CP_ESP_IDF_COMPONENTS = {"cameraif": ("esp_cam_sensor", "esp_sccb_intf", "cmake_utilities")}
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
    named (palettes, pdwidgets, lvgl-circuitpython's fs_driver, ...), and the
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


def refuse_lvgl_micropython(names):
    """lvgl-micropython is MicroPython's LVGL binding. CircuitPython compiles
    with -Wshadow -Werror, which its generated binding trips over, so a build
    that reached that error would only say 'shadows a global declaration'.
    CircuitPython's binding is lvgl-circuitpython, given to --modules by path."""
    if LVGL_MODULE in names:
        die(
            f"{LVGL_MODULE} is MicroPython's LVGL binding and does not build on CircuitPython "
            "(its generated code stops on -Werror=shadow). For --interpreter circuitpython use "
            "lvgl-circuitpython, passed by path: --modules /path/to/lvgl-circuitpython "
            "(see its docs/build-and-flash.md)"
        )


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
    # Before anything is fetched: a plain name or a path ending in the name.
    refuse_lvgl_micropython([Path(i.strip()).name for i in (args.modules or "").split(",")])
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
    spec, module_dirs, library_packages = resolve_modules(spec, library)
    refuse_lvgl_micropython([module_name(d) for d in module_dirs])
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
    bindings = lvgl_bindings(ws, module_dirs)
    revisions = record(module_dirs, bindings, library.source)
    rec_path = build_dir_for(build, want, args.clean, revisions)
    user_c = cp_user_c_modules(module_dirs, build / "usermods")
    frozen, frozen_args = (None, []) if any(a.startswith("FROZEN_MANIFEST=") for a in make_extra) else \
        cp_frozen_manifest(cp, port, board, variant, module_dirs, library_packages, build, library)

    env = {k: v for k, v in os.environ.items()
           if k not in ("USER_C_MODULES", "FROZEN_MANIFEST", "BUILD", "BOARD", "VARIANT", "VARIANT_DIR", "PYTHONPATH",
                        "BINDINGS_DIR")}
    if bindings:
        env["BINDINGS_DIR"] = str(bindings.resolve())
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
    ap.add_argument("--no-autosize", action="store_true", help="esp32: refuse instead of growing the app partition")
    ap.add_argument("--coredump", action="store_true",
                    help="esp32: a troubleshooting build that writes a core dump to flash on a crash (standard builds don't)")
    ap.add_argument("--clean", action="store_true", help="delete this target's build dir first")
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
    upstream = (REPO / "UPSTREAM").read_text().strip()
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
    library = Library(ws)
    spec, module_dirs, library_packages = resolve_modules(spec, library)
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
    # Each module's commit too: a module that moved keeps its name, and make
    # would reuse frozen content and qstr headers from before it moved.
    bindings = lvgl_bindings(ws, module_dirs)
    revisions = record(module_dirs, bindings, library.source)
    build_dir_for(build, want, args.clean, revisions)

    env = dict(os.environ)
    for leak in ("USER_C_MODULES", "FROZEN_MANIFEST", "BUILD", "BOARD", "VARIANT", "BOARD_DIR", "VARIANT_DIR",
                 "BINDINGS_DIR"):
        env.pop(leak, None)
    env["PYDEVICES_MODULES"] = spec
    # The frozen pydevices leaves its host-only files off a board's image.
    env["PYDEVICES_PLATFORM"] = "desktop" if port in ("unix", "windows", "webassembly") else "board"
    if version:
        # makeversionhdr.py takes these instead of asking git, which a shallow clone can't answer.
        env["MICROPY_GIT_TAG"], env["MICROPY_GIT_HASH"] = version
    if library.tree:
        env["PYDEVICES_LIBRARY"] = library.tree.as_posix()
    if bindings:
        env["BINDINGS_DIR"] = bindings.resolve().as_posix()
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
    roots = module_dirs + ([bindings] if bindings else []) + [VARIANTS_DIR / port]
    for dep, src in foreign_sources(build, roots):
        say(f"compiling {dep.relative_to(build).with_suffix('.o').as_posix()} again: it was compiled from {src}")
        dep.with_suffix(".o").unlink(missing_ok=True)
        dep.unlink()

    rc, out = 0, ""
    tinyusb = None
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
                say("  Shrinking the filesystem moves it: a board flashed with this image comes up")
                say("  with an empty one. Copy your files back after flashing, or build with")
                say("  --no-autosize to stop here instead.\n")
                # The port runs CMake only when the build has no Makefile yet.
                (build / "Makefile").unlink(missing_ok=True)
            # What the image was built with, grown now or on an earlier run (CMake caches it).
            cache = build / "CMakeCache.txt"
            got = re.search(r"^MICROPY_HW_FLASH_STORAGE_BYTES:\w+=(\d+)$", cache.read_text(), re.M) if cache.exists() else None
            rp2_storage = int(got.group(1)) if rc == 0 and got else None
        else:
            shell(make + ["submodules"])
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
    rec = dict(want, complete=True)
    if port == "rp2" and rp2_storage:
        rec["rp2_storage_bytes"] = rp2_storage
    rec["micropython"] = git_out(mp, "describe", "--always", "--abbrev=12")
    if version:
        rec["micropython_version"] = version[0]
    rec["module_revisions"] = revisions
    rec["micropython_lib"] = apply_patches.lib_record(mp)
    if port == "esp32" and tinyusb:
        rec["tinyusb"] = git_out(tinyusb, "describe", "--tags", "--always")
    rec_path.write_text(json.dumps(rec, indent=1) + "\n")
    say(f"\nBuilt into {build}")
    for name in ("firmware.bin", "firmware.uf2", "micropython", "micropython.exe", "micropython.mjs", "micropython.wasm"):
        if (build / name).exists():
            say(f"  {build / name}")


if __name__ == "__main__":
    main()
