#!/usr/bin/env python3
"""The firmware set: which boards, how each is built, and the checks on the result.

.github/workflows/release-firmware.yml calls this; every step also runs by hand
on Linux or WSL.

    release_firmware.py matrix [--pico2]
        Print the build matrix as JSON for the workflow: one row per cell.
        The Pico 2's row is left out unless --pico2 is given.

    release_firmware.py build <cell> <out-dir> [--ws DIR]
        Build one cell with build_mp.py and put its files, a sha256 of each and
        a cell.json in <out-dir>/<cell>/. --ws is a directory holding a
        checkout of lvgl-circuitpython, which the CircuitPython cells compile
        in.

    release_firmware.py assemble <in-dir> <out-dir> [--pico2] [--tag TAG]
                                 [--run-url URL] [--lvgl-circuitpython-commit SHA]
        Check every cell in <in-dir> (one directory per cell, as `build`
        wrote them), name the files for the set and write firmware.json
        beside them in <out-dir>. Exits 1 if any check fails. It never
        uploads or publishes anything.

    release_firmware.py notes <firmware.json> [--out FILE]
        Check that the set's tag may go on a draft Release and write the
        Release's notes (to FILE, or stdout). A set built from preview pins
        only takes a tag that starts with "staged-", so publishing it by
        accident would look wrong. This is what the workflow's draft step runs.

    release_firmware.py cells
        List the cells.

The checks in `assemble` are the ones a download would trip over: each file is
the one the build wrote (sha256), none is empty or past its flash, a flash
image starts like one and carries the app image the build also produced, a UF2
is whole blocks, no module was built from a dirty tree, and every cell the run
should have built is there.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

MB = 1024 * 1024

# A cell is one file set for one board. `chip` and `flash` are what the flash
# image is written with: `offset` is the flash address of the merged image's
# first byte (MicroPython's P4 image starts at its bootloader, 0x2000;
# CircuitPython's is padded from 0). The bootloader itself is at BOOTLOADER.
# `deps` names the cache the workflow keeps its toolchains in.
CELLS = [
    dict(id="mp-p4-devkit", label="ESP32-P4-WIFI6-DEV-KIT", interpreter="micropython", port="esp32",
         board="ESP32_GENERIC_P4", variant="WIFI6_DEV_KIT_H2", flash="16MB", modules="all",
         chip="esp32p4", offset=0x2000, kind="esp32", deps="idf"),
    dict(id="mp-p4-panel", label="ESP32-P4-WIFI6-Touch-LCD-4B", interpreter="micropython", port="esp32",
         board="ESP32_GENERIC_P4", variant="PRE_REV3_C6_WIFI", flash="32MB", modules="all",
         chip="esp32p4", offset=0x2000, kind="esp32", deps="idf"),
    dict(id="mp-s3-lcd7", label="ESP32-S3-Touch-LCD-7", interpreter="micropython", port="esp32",
         board="ESP32_GENERIC_S3", variant="LCD_7", flash="8MB", modules="all",
         chip="esp32s3", offset=0, kind="esp32", deps="idf"),
    dict(id="mp-s3-lcd43", label="ESP32-S3-Touch-LCD-4.3", interpreter="micropython", port="esp32",
         board="ESP32_GENERIC_S3", variant="LCD_4_3", flash="8MB", modules="all",
         chip="esp32s3", offset=0, kind="esp32", deps="idf"),
    dict(id="mp-s3-tembed", label="LilyGO T-Embed S3", interpreter="micropython", port="esp32",
         board="ESP32_GENERIC_S3", variant="T_EMBED", flash="16MB", modules="all",
         chip="esp32s3", offset=0, kind="esp32", deps="idf"),
    dict(id="cp-p4-devkit", label="ESP32-P4-WIFI6-DEV-KIT", interpreter="circuitpython", port="espressif",
         board="waveshare_esp32p4_wifi6_dev_kit", variant=None, flash="16MB",
         modules="audiodsp,pygraphics,pngio,tsmux,ppa,jpegio,cameraif,castif,h264enc,usbhost,pydevices,pdwidgets,palettes,@lvgl-circuitpython",
         chip="esp32p4", offset=0, kind="espressif", deps="cp-esp"),
    dict(id="cp-s3-lcd7", label="ESP32-S3-Touch-LCD-7", interpreter="circuitpython", port="espressif",
         board="waveshare_esp32s3_touch_lcd_7", variant=None, flash="8MB",
         modules="audiodsp,pygraphics,pngio,tsmux,pydevices,pdwidgets,palettes,@lvgl-circuitpython",
         chip="esp32s3", offset=0, kind="espressif", deps="cp-esp"),
    dict(id="cp-s3-lcd43", label="ESP32-S3-Touch-LCD-4.3", interpreter="circuitpython", port="espressif",
         board="waveshare_esp32s3_touch_lcd_43", variant=None, flash="8MB",
         modules="audiodsp,pygraphics,pngio,tsmux,pydevices,pdwidgets,palettes,@lvgl-circuitpython",
         chip="esp32s3", offset=0, kind="espressif", deps="cp-esp"),
    dict(id="cp-twatch", label="LilyGO T-Watch S3", interpreter="circuitpython", port="espressif",
         board="lilygo_twatch_s3_single_app", variant=None, flash="16MB",
         modules="audiodsp,pygraphics,pngio,tsmux,pydevices,pdwidgets,palettes,@lvgl-circuitpython",
         chip="esp32s3", offset=0, kind="espressif", deps="cp-esp"),
    dict(id="cp-feather-rp2040", label="Adafruit Feather RP2040 + display wing", interpreter="circuitpython",
         port="raspberrypi", board="feather_rp2040_lvgl", variant=None, flash="8MB",
         modules="pygraphics,pngio,tsmux,pydevices,pdwidgets,palettes,@lvgl-circuitpython", chip="rp2040", offset=None, kind="rp2", deps="none"),
    # Off by default until a board is on the bench to run it on.
    dict(id="mp-pico2", label="Raspberry Pi Pico 2", interpreter="micropython", port="rp2",
         board="RPI_PICO2", variant=None, flash="4MB", modules="all",
         chip="rp2350", offset=None, kind="rp2", deps="none", optional=True),
]

# What each kind of cell must hand over, as (role, file in the build dir, required).
# `flash` is the one file a user writes; `app` is the application image inside it.
FILES = {
    ("micropython", "esp32"): [("flash", "firmware.bin", True), ("app", "micropython.bin", True),
                               ("elf", "micropython.elf", True), ("map", "micropython.map", True),
                               ("partitions", "partition_table/partition-table.bin", True)],
    ("circuitpython", "espressif"): [("flash", "firmware.bin", True), ("app", "circuitpython-firmware.bin", True),
                                     ("elf", "firmware.elf", True), ("map", "firmware.elf.map", True)],
    ("circuitpython", "rp2"): [("uf2", "firmware.uf2", True), ("elf", "firmware.elf", True),
                               ("map", "firmware.elf.map", True)],
    ("micropython", "rp2"): [("uf2", "firmware.uf2", True), ("elf", "firmware.elf", True),
                             ("map", "firmware.elf.map", True)],
}

EXTENSION = {"flash": ".bin", "app": ".app.bin", "uf2": ".uf2", "elf": ".elf", "map": ".map",
             "partitions": ".partitions.bin"}


class Failed(Exception):
    pass


def cell(cell_id):
    for c in CELLS:
        if c["id"] == cell_id:
            return c
    raise Failed(f"no cell '{cell_id}' (have: {', '.join(c['id'] for c in CELLS)})")


def wanted(pico2=False):
    return [c for c in CELLS if pico2 or not c.get("optional")]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def read_lock(name):
    rows = []
    for line in (REPO / name).read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            rows.append(line.split())
    return rows


# ---- matrix ----------------------------------------------------------------

def matrix(pico2):
    rows = [{k: c[k] for k in ("id", "label", "kind", "deps", "interpreter")} for c in wanted(pico2)]
    return {"include": rows}


# ---- build -----------------------------------------------------------------

def micropython_version():
    """The checkout's own version, vX.Y.Z with -preview while upstream says so.
    It names the set's files; the image's own sys.version tag is checked apart."""
    h = (REPO / "micropython" / "py" / "mpconfig.h").read_text()
    nums = [re.search(rf"define MICROPY_VERSION_{k} (\d+)", h).group(1) for k in ("MAJOR", "MINOR", "MICRO")]
    pre = int(re.search(r"define MICROPY_VERSION_PRERELEASE (\d+)", h).group(1))
    return "v" + ".".join(nums) + ("-preview" if pre else "")


def check_version_tag(tag, elf, cell_id, after=b""):
    """The image has to print a version. A build that couldn't ask git for one
    (a clone without tags) prints the commit's hash instead, and still links.
    `after` is what follows the version where the image prints it: a
    CircuitPython version that gained a -dirty contains the clean one."""
    if not tag:
        raise Failed(f"{cell_id}: the build recorded no version, so its sys.version would show a commit hash")
    if tag.encode() + after not in Path(elf).read_bytes():
        raise Failed(f"{cell_id}: {Path(elf).name} does not contain the version {tag}")


def check_frozen_libraries(libraries, elf, cell_id):
    """A CircuitPython image that freezes our Python has to carry asyncio and
    the clock under it, adafruit_ticks: bledev and the rest import asyncio, and
    no CircuitPython board freezes one. The build record names the libraries it
    froze, and the image has to hold their files."""
    names = " ".join(libraries or [])
    for lib in ("Adafruit_CircuitPython_asyncio", "Adafruit_CircuitPython_Ticks"):
        if lib not in names:
            raise Failed(f"{cell_id}: the build record names no frozen {lib}")
    data = Path(elf).read_bytes()
    for module in (b"asyncio/core.py", b"adafruit_ticks.py"):
        if module not in data:
            raise Failed(f"{cell_id}: {Path(elf).name} does not contain the frozen {module.decode()}")


def build_dir(c):
    if c["interpreter"] == "circuitpython":
        return REPO / "builds" / "circuitpython" / c["port"] / c["board"]
    return REPO / "builds" / c["port"] / c["board"] / (c["variant"] or "default")


def build_args(c, ws):
    args = [sys.executable, str(REPO / "build_mp.py"), "--port", c["port"], "--board", c["board"]]
    if c["interpreter"] == "circuitpython":
        args[2:2] = ["--interpreter", "circuitpython"]
    if c["variant"]:
        args += ["--variant", c["variant"]]
    if c["interpreter"] == "micropython" and c["port"] == "esp32":
        args += ["--flash", c["flash"]]
    mods = []
    for m in c["modules"].split(","):
        if m.startswith("@"):
            if not ws:
                raise Failed(f"{c['id']} compiles in {m[1:]}: give --ws a directory holding it")
            m = str(Path(ws, m[1:]).resolve())
        mods.append(m)
    args += ["--modules", ",".join(mods)]
    return args


def build(cell_id, out, ws):
    c = cell(cell_id)
    out = Path(out) / c["id"]
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    args = build_args(c, ws)
    print("+", " ".join(args), flush=True)
    start = time.time()
    rc = subprocess.run(args, cwd=REPO).returncode
    seconds = round(time.time() - start)
    if rc != 0:
        raise Failed(f"build_mp.py failed for {c['id']} (exit {rc}) after {seconds} s")
    src = build_dir(c)
    record = json.loads((src / "pydevices-build.json").read_text())
    if not record.get("complete"):
        raise Failed(f"{src / 'pydevices-build.json'} says the build is not complete")

    files = {}
    for role, name, required in FILES[(c["interpreter"], c["kind"])]:
        f = src / name
        if not f.is_file():
            if required:
                raise Failed(f"{c['id']}: the build left no {name} in {src}")
            continue
        dest = out / f"{role}{EXTENSION[role]}"
        shutil.copyfile(f, dest)
        files[role] = {"file": dest.name, "size": dest.stat().st_size, "sha256": sha256(dest)}
    shutil.copyfile(src / "pydevices-build.json", out / "build-record.json")
    if c["interpreter"] == "micropython":
        check_version_tag(record.get("micropython_version"), out / f"elf{EXTENSION['elf']}", c["id"])
        print(f"  sys.version tag: {record['micropython_version']}", flush=True)

    if c["interpreter"] == "circuitpython":
        # The banner and os.uname() print "<version> on <date>".
        check_version_tag(record.get("circuitpython_version"), out / f"elf{EXTENSION['elf']}", c["id"], b" on ")
        print(f"  version: {record['circuitpython_version']}", flush=True)
        check_frozen_libraries(record.get("frozen_libraries"), out / f"elf{EXTENSION['elf']}", c["id"])
        print(f"  frozen libraries: {', '.join(record['frozen_libraries'])}", flush=True)

    meta = {k: c[k] for k in ("id", "label", "interpreter", "port", "board", "variant", "chip", "flash", "modules")}
    meta["flash_offset"] = None if c["offset"] is None else hex(c["offset"])
    meta["flash_bytes"] = int(c["flash"][:-2]) * MB
    meta["kind"] = c["kind"]
    if c["interpreter"] == "circuitpython":
        meta["frozen_libraries"] = record["frozen_libraries"]
    meta["files"] = files
    meta["build_seconds"] = seconds
    meta["interpreter_version"] = (micropython_version() if c["interpreter"] == "micropython"
                                   else record.get("circuitpython_version"))
    meta["module_revisions"] = {n: m for n, m in sorted((record.get("module_revisions") or {}).items())}
    if c["interpreter"] == "circuitpython":
        meta["circuitpython_patches"] = record.get("circuitpython_patches", [])
    if c["kind"] == "esp32":
        args_json = json.loads((src / "flasher_args.json").read_text())
        meta["esptool"] = {"chip": args_json.get("extra_esptool_args", {}).get("chip"),
                           "flash_settings": args_json.get("flash_settings", {}),
                           "flash_files": args_json.get("flash_files", {})}
    (out / "cell.json").write_text(json.dumps(meta, indent=1) + "\n")
    print(f"{c['id']}: built in {seconds} s into {out}")
    for role, info in files.items():
        print(f"  {role:12} {info['size']:>10}  {info['file']}")


# ---- checks ----------------------------------------------------------------

ESP_IMAGE_MAGIC = 0xE9
BOOTLOADER = {"esp32s3": 0x0, "esp32p4": 0x2000}
UF2_MAGICS = (b"UF2\n", b"WQ]\x9e", b"\x30\x6f\xb1\x0a")  # start magic 0, 1 and the end magic
# Smallest sizes a firmware that carries this interpreter and its modules can have.
MIN_BYTES = {"esp32": 1 * MB, "espressif": 1 * MB, "rp2": 512 * 1024}


def check_cell(d, c):
    """(problems, cell.json) for one cell's directory; no problems means it passed."""
    problems = []
    try:
        meta = json.loads((d / "cell.json").read_text())
    except (OSError, ValueError) as e:
        return [f"{c['id']}: no readable cell.json ({e})"], None
    for key in ("board", "variant", "interpreter", "chip", "flash", "modules"):
        if meta.get(key) != c[key]:
            problems.append(f"{c['id']}: built as {key}={meta.get(key)!r}, the cell says {c[key]!r}")
    files = meta.get("files", {})
    want = [(role, req) for role, _, req in FILES[(c["interpreter"], c["kind"])]]
    for role, required in want:
        if required and role not in files:
            problems.append(f"{c['id']}: no {role} file")
    data = {}
    for role, info in files.items():
        f = d / info["file"]
        if not f.is_file():
            problems.append(f"{c['id']}: {info['file']} is missing")
            continue
        size = f.stat().st_size
        if size == 0:
            problems.append(f"{c['id']}: {info['file']} is empty")
        if size != info["size"]:
            problems.append(f"{c['id']}: {info['file']} is {size} bytes, the build wrote {info['size']}")
        got = sha256(f)
        if got != info["sha256"]:
            problems.append(f"{c['id']}: {info['file']} sha256 {got[:12]} is not the build's {info['sha256'][:12]}")
        data[role] = f

    flash_bytes = meta.get("flash_bytes", 0)
    # A flash image: starts like one, fits the flash, carries the app image.
    if "flash" in data:
        img = data["flash"].read_bytes()
        offset = c["offset"]
        if len(img) < MIN_BYTES[c["kind"]]:
            problems.append(f"{c['id']}: {data['flash'].name} is only {len(img)} bytes")
        if offset + len(img) > flash_bytes:
            problems.append(f"{c['id']}: {data['flash'].name} ends at {offset + len(img)}, past the {c['flash']} flash")
        boot = BOOTLOADER[c["chip"]] - offset
        if boot < 0 or len(img) <= boot or img[boot] != ESP_IMAGE_MAGIC:
            problems.append(f"{c['id']}: {data['flash'].name} has no ESP image magic 0xE9 at the {c['chip']} "
                            f"bootloader's {hex(BOOTLOADER[c['chip']])}")
        if "app" in data:
            app = data["app"].read_bytes()
            at = img.find(app[:65536])
            if at < 0 or img[at:at + len(app)] != app:
                problems.append(f"{c['id']}: the app image is not inside {data['flash'].name}")
            else:
                meta["app_offset"] = hex(offset + at)
                if len(app) > len(img) - at:
                    problems.append(f"{c['id']}: the app image runs past the end of {data['flash'].name}")
    if "uf2" in data:
        blob = data["uf2"].read_bytes()
        if len(blob) < MIN_BYTES[c["kind"]]:
            problems.append(f"{c['id']}: {data['uf2'].name} is only {len(blob)} bytes")
        if len(blob) % 512:
            problems.append(f"{c['id']}: {data['uf2'].name} is not whole 512-byte blocks")
        elif blob[:4] != UF2_MAGICS[0] or blob[4:8] != UF2_MAGICS[1] or blob[-4:] != UF2_MAGICS[2]:
            problems.append(f"{c['id']}: {data['uf2'].name} has no UF2 block magic")
        elif len(blob) // 512 * 256 > flash_bytes:
            problems.append(f"{c['id']}: {data['uf2'].name} carries more than the {c['flash']} flash holds")
    for role in ("elf", "map"):
        if role in data and data[role].stat().st_size < 1024:
            problems.append(f"{c['id']}: {data[role].name} is only {data[role].stat().st_size} bytes")
    if "elf" in data and data["elf"].read_bytes()[:4] != b"\x7fELF":
        problems.append(f"{c['id']}: {data['elf'].name} is not an ELF file")
    for name, rev in (meta.get("module_revisions") or {}).items():
        if rev.get("dirty"):
            problems.append(f"{c['id']}: module {name} was built from a dirty tree")
        if not re.fullmatch(r"[0-9a-f]{40}", rev.get("commit") or ""):
            problems.append(f"{c['id']}: module {name} has no full commit ({rev.get('commit')!r})")
    return problems, meta


def version_numbers(describe):
    m = re.match(r"v?(\d+\.\d+\.\d+)", describe or "")
    return m.group(1) if m else None


def preview_reasons(upstream_describe, cp_version, deps):
    """Why a set built from these pins is a preview (D3: such a set is never published)."""
    reasons = []
    if "preview" in (upstream_describe or "") or re.search(r"-\d+-g[0-9a-f]+$", upstream_describe or ""):
        reasons.append(f"MicroPython is {upstream_describe}, not a tagged release")
    if cp_version and re.search(r"(alpha|beta|rc)", cp_version):
        reasons.append(f"CircuitPython is {cp_version}, a pre-release")
    return reasons


def assemble(indir, out, pico2, tag, run_url, lvgl_cp_commit, now=None):
    indir, out = Path(indir), Path(out)
    now = now or datetime.datetime.now(datetime.timezone.utc)
    date = now.strftime("%Y%m%d")
    problems, metas = [], {}
    for c in wanted(pico2):
        d = indir / c["id"]
        if not d.is_dir():
            problems.append(f"{c['id']}: not built (no {d})")
            continue
        probs, meta = check_cell(d, c)
        problems += probs
        if meta:
            metas[c["id"]] = meta
    for extra in sorted(p.name for p in indir.iterdir() if p.is_dir()) if indir.is_dir() else []:
        if extra not in {c["id"] for c in wanted(True)}:
            problems.append(f"{extra}: a directory that is no cell")
    if problems:
        raise Failed("\n".join(problems))

    mp_versions = {m["interpreter_version"] for m in metas.values() if m["interpreter"] == "micropython"}
    cp_versions = {m["interpreter_version"] for m in metas.values() if m["interpreter"] == "circuitpython"}
    if len(mp_versions) > 1:
        raise Failed(f"the MicroPython cells disagree about what they built: {sorted(mp_versions)}")
    mp_version = mp_versions.pop() if mp_versions else None
    cp_version = re.sub(r"-dirty$", "", cp_versions.pop()) if len(cp_versions) == 1 else None
    nums = version_numbers(mp_version)
    tag = tag or (f"v{nums}-{date}" if nums else f"unreleased-{date}")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", tag):
        raise Failed(f"'{tag}' is not a usable tag")

    out.mkdir(parents=True, exist_ok=True)
    builds = []
    for c in wanted(pico2):
        m = metas[c["id"]]
        d = indir / c["id"]
        kind = "mp" if c["interpreter"] == "micropython" else "cp"
        stem = "-".join(p.lower() for p in (c["board"], c["variant"]) if p)
        if kind == "mp":
            base = f"omnipython-mp-{stem}-{tag}"
        else:
            base = f"omnipython-cp-{stem}-{re.sub(r'-dirty$', '', m['interpreter_version'])}-{date}"
        files = []
        for role, info in m["files"].items():
            name = base + EXTENSION[role]
            shutil.copyfile(d / info["file"], out / name)
            files.append({"role": role, "name": name, "size": info["size"], "sha256": info["sha256"]})
        entry = {k: m[k] for k in ("id", "label", "interpreter", "interpreter_version", "board", "variant",
                                   "chip", "flash", "modules", "flash_offset", "build_seconds",
                                   "module_revisions")}
        if m.get("app_offset"):
            entry["app_offset"] = m["app_offset"]
        if "esptool" in m:
            entry["esptool"] = m["esptool"]
        if "circuitpython_patches" in m:
            entry["circuitpython_patches"] = m["circuitpython_patches"]
        entry["files"] = files
        builds.append(entry)

    deps = {r[0]: r[2] for r in read_lock("deps.lock")}
    pins = {"UPSTREAM": (REPO / "UPSTREAM").read_text().strip(),
            "deps.lock": deps,
            "modules.lock": {r[0]: r[2] for r in read_lock("modules.lock")}}
    if lvgl_cp_commit:
        pins["lvgl-circuitpython"] = lvgl_cp_commit
    reasons = preview_reasons(mp_version, cp_version, deps)
    doc = {"schema": 1, "tag": tag, "built": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
           "run": run_url, "micropython": mp_version, "circuitpython": cp_version,
           "preview": bool(reasons), "preview_reasons": reasons, "pins": pins, "builds": builds}
    (out / "firmware.json").write_text(json.dumps(doc, indent=1) + "\n")
    return doc


def summary(doc):
    lines = [f"Firmware set {doc['tag']}" + (" (preview pins: never published)" if doc["preview"] else ""),
             "", "| Cell | Build | Flash image | Offset | Build time |", "|---|---|---:|---|---:|"]
    for b in doc["builds"]:
        main = next(f for f in b["files"] if f["role"] in ("flash", "uf2"))
        lines.append(f"| {b['id']} | {b['label']} | {main['size'] / MB:.2f} MB | {b.get('flash_offset') or 'UF2'} "
                     f"| {b['build_seconds'] // 60} min {b['build_seconds'] % 60:02d} s |")
    return "\n".join(lines) + "\n"


# ---- the draft Release -----------------------------------------------------

REPO_SLUG = "PyDevices/micropython-pydevices"
STAGED_TAG = re.compile(r"staged-[A-Za-z0-9][A-Za-z0-9._-]*")
FINAL_TAG = re.compile(r"v\d+\.\d+\.\d+-\d{8}")


def check_draft_tag(doc):
    """A set built from preview pins (D3) only gets a tag that says it is staged."""
    tag = doc["tag"]
    if doc["preview"]:
        if not STAGED_TAG.fullmatch(tag):
            raise Failed(f"this set is built from preview pins ({'; '.join(doc['preview_reasons'])}), "
                         f"so its tag must start with 'staged-', not '{tag}'")
    elif not (FINAL_TAG.fullmatch(tag) or STAGED_TAG.fullmatch(tag)):
        raise Failed(f"'{tag}' is neither v<MicroPython version>-<YYYYMMDD> nor staged-<name>")


def write_command(b, f):
    """The esptool line that writes one ESP flash image, or None for a file that isn't one."""
    if f["role"] != "flash":
        return None
    return f"esptool --chip {b['chip']} write-flash {b['flash_offset']} {f['name']}"


def notes(doc):
    """The draft Release's body, for whoever opens it."""
    tag = doc["tag"]
    lines = []
    if doc["preview"]:
        lines += ["> **Staged, not for release.** " + "; ".join(doc["preview_reasons"]) +
                  ". This draft stays private until a set is built from tagged versions.", ""]
    lines += [f"OmniPython firmware {tag}: MicroPython {doc['micropython']}"
              + (f" and CircuitPython-compatible {doc['circuitpython']}" if doc.get("circuitpython") else "")
              + f", built by [this run]({doc['run']}) on {doc['built'][:10]}.", "",
              "**Before you flash,** copy your files off the board, `secrets.py` above all. A build can move the "
              "filesystem, and a flash across a layout change leaves it empty. Coming from another firmware "
              "(MicroPython to CircuitPython or back), run `esptool --chip <chip> erase-flash` first.", "",
              "| Board | Interpreter | File to flash | How |", "|---|---|---|---|"]
    for b in doc["builds"]:
        f = next(x for x in b["files"] if x["role"] in ("flash", "uf2"))
        how = ("copy it onto the board's BOOTSEL drive" if f["role"] == "uf2"
               else f"`{write_command(b, f)}`")
        kind = "MicroPython" if b["interpreter"] == "micropython" else "CircuitPython-compatible"
        lines.append(f"| {b['label']} | {kind} {b['interpreter_version']} | `{f['name']}` | {how} |")
    lines += ["",
              "Each build also ships its `.elf` and `.map`, and the ESP32 builds their `.app.bin` (the application "
              "alone, for a board that keeps its other partitions) and partition table. The `.elf` that matches a "
              "flashed build is what turns a crash report or core dump into a source-level backtrace. "
              "`firmware.json` lists every file's sha256, size, flash offset and the pins the set was built from.", "",
              "Every file here is attested. Check one with:", "",
              "```",
              f"gh attestation verify <file> --repo {REPO_SLUG}",
              "```", ""]
    return "\n".join(lines)


# ---- command line ----------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("matrix")
    m.add_argument("--pico2", action="store_true")
    b = sub.add_parser("build")
    b.add_argument("cell")
    b.add_argument("out")
    b.add_argument("--ws")
    a = sub.add_parser("assemble")
    a.add_argument("indir")
    a.add_argument("out")
    a.add_argument("--pico2", action="store_true")
    a.add_argument("--tag", default="")
    a.add_argument("--run-url", default="")
    a.add_argument("--lvgl-circuitpython-commit", default="")
    n = sub.add_parser("notes")
    n.add_argument("firmware_json")
    n.add_argument("--out")
    sub.add_parser("cells")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "matrix":
            print(json.dumps(matrix(args.pico2), separators=(",", ":")))
        elif args.cmd == "cells":
            for c in CELLS:
                print(f"{c['id']:20} {c['interpreter']:14} {c['board']} {c['variant'] or ''}"
                      f"{'  (off by default)' if c.get('optional') else ''}")
        elif args.cmd == "notes":
            doc = json.loads(Path(args.firmware_json).read_text())
            check_draft_tag(doc)
            text = notes(doc)
            if args.out:
                Path(args.out).write_text(text)
            else:
                print(text)
        elif args.cmd == "build":
            build(args.cell, args.out, args.ws)
        elif args.cmd == "assemble":
            doc = assemble(args.indir, args.out, args.pico2, args.tag, args.run_url, args.lvgl_circuitpython_commit)
            text = summary(doc)
            print(text)
            if os.environ.get("GITHUB_STEP_SUMMARY"):
                with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
                    f.write(text)
    except Failed as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
