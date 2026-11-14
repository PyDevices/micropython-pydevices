#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""The firmware set: which boards, how each is built, and the checks on the result.

.github/workflows/release-firmware.yml calls this; every step also runs by hand
on Linux or WSL.

    release_firmware.py matrix [--pico2]
        Print the build matrix as JSON for the workflow: one row per cell.
        The Pico 2's and the Pico Plus 2 W's rows are left out unless --pico2 is given.

    release_firmware.py build <cell> <out-dir>
        Build one cell with build_mp.py and put its files, a sha256 of each and
        a cell.json in <out-dir>/<cell>/. LVGL (omnipython-lvgl) comes in at
        the commit DEPENDENCIES.lock pins, for MicroPython and CircuitPython alike.

    release_firmware.py assemble <in-dir> <out-dir> [--pico2] [--tag TAG]
                                 [--run-url URL]
        Check every cell in <in-dir> (one directory per cell, as `build`
        wrote them), name the files for the set and write firmware.json
        beside them in <out-dir>. Exits 1 if any check fails. It never
        uploads or publishes anything.

    release_firmware.py notes <firmware.json> [--previous FILE] [--out FILE]
        Check that the set's tag may go on a draft Release and write the
        Release's notes (to FILE, or stdout). A set built from preview pins
        only takes a tag that starts with "staged-", so publishing it by
        accident would look wrong. With --previous, the firmware.json of the
        last set people could download, the notes name every board whose
        filesystem this set moves. This is what the workflow's draft step runs.

    release_firmware.py cells
        List the cells.

The checks in `assemble` are the ones a download would trip over: each file is
the one the build wrote (sha256), none is empty or past its flash, a flash
image starts like one and carries the app image the build also produced, a UF2
is whole blocks, no module was built from a dirty tree, every cell the run
should have built is there, and each MicroPython board's filesystem is where
its cell keeps it.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
import pins as pinfile  # noqa: E402

MB = 1024 * 1024

# A cell is one file set for one board. `chip` and `flash` are what the flash
# image is written with: `offset` is the flash address of the merged image's
# first byte (MicroPython's P4 image starts at its bootloader, 0x2000;
# CircuitPython's is padded from 0). The bootloader itself is at BOOTLOADER.
# `deps` names the cache the workflow keeps its toolchains in. An stm32 cell
# also has `base`, where the chip's flash starts: `offset` is an address there,
# and the Wi-Fi firmware goes to WIFI_OFFSET in the QSPI flash.
#
# `fs` is the filesystem a MicroPython board keeps from one release to the next,
# the end of its flash, which build_mp.py builds beside (--keep-fs). An image
# that outgrows the flash left to it fails its build instead of shipping a
# filesystem that moved: a board that updated to that would come up with an
# empty one. Changing `fs` moves the filesystem on that board, so it is done on
# purpose, and the notes of the release that ships it say so (`notes --previous`).
# A cell with `autosize=True` and no `fs` lets build_mp.py grow the app slot to
# the image instead, so the filesystem is whatever flash is left: it can move
# with any release, and the notes say so whenever it does.
CELLS = [
    # One image for the revision 3 P4 boards with a C6 on the DEV-KIT's pins: the DEV-KIT (16 MB) and
    # the Pico-sized ESP32-P4-WIFI6 (32 MB). The size of PSRAM is read at boot, and a 16 MB image is
    # the one a 16 MB board can start (an image written for 32 MB doesn't start on 16), so a 32 MB
    # board runs it and keeps the table it has when you write only the app.
    dict(id="mp-p4-devkit", label="ESP32-P4-WIFI6-DEV-KIT and ESP32-P4-WIFI6 (Pico-sized)", interpreter="micropython", port="esp32",
         board="ESP32_GENERIC_P4", variant="WIFI6_DEV_KIT_H2", flash="16MB", modules="all",
         chip="esp32p4", offset=0x2000, kind="esp32", deps="idf", fs="10176K"),
    # A P4 before chip revision 3: the bootloader refuses a revision 3 image on it, and a revision 3
    # chip refuses this one.
    dict(id="mp-p4-panel", label="ESP32-P4-WIFI6-Touch-LCD-4B (P4 before revision 3)", interpreter="micropython", port="esp32",
         board="ESP32_GENERIC_P4", variant="PRE_REV3_C6_WIFI", flash="32MB", modules="all",
         chip="esp32p4", offset=0x2000, kind="esp32", deps="idf", fs="26624K"),
    # The P4X-EYE wires its C6 to other pins than the DEV-KIT's (compiled into the image) and puts the
    # REPL on the USB Serial/JTAG port the DEV-KIT's image moves, so it can't share one.
    dict(id="mp-p4-eye", label="ESP32-P4X-EYE", interpreter="micropython", port="esp32",
         board="ESP32_GENERIC_P4", variant="P4X_EYE", flash="16MB", modules="all,vision",
         chip="esp32p4", offset=0x2000, kind="esp32", deps="idf", fs="7104K"),
    dict(id="mp-s3-lcd7", label="ESP32-S3-Touch-LCD-7", interpreter="micropython", port="esp32",
         board="ESP32_GENERIC_S3", variant="LCD_7", flash="8MB", modules="all",
         chip="esp32s3", offset=0, kind="esp32", deps="idf", autosize=True),
    dict(id="mp-s3-lcd43", label="ESP32-S3-Touch-LCD-4.3", interpreter="micropython", port="esp32",
         board="ESP32_GENERIC_S3", variant="LCD_4_3", flash="8MB", modules="all",
         chip="esp32s3", offset=0, kind="esp32", deps="idf", autosize=True),
    # The generic S3 images, for the boards that have no cell of their own. Each is a plain
    # upstream board with our S3 settings, no board variant.
    # Octal PSRAM, 16 MB flash: Adafruit Qualia ESP32-S3, LilyGO T-Display-S3, Sunton/LilyGO T-HMI,
    # WT32-SC01 Plus (with 8 MB flash it fits too), Unexpected Maker FeatherS3 and other
    # octal-PSRAM S3 boards.
    dict(id="mp-s3-octal", label="ESP32-S3 with octal PSRAM (Qualia ESP32-S3, T-Display-S3, T-HMI, WT32-SC01 Plus, FeatherS3)",
         interpreter="micropython", port="esp32", board="ESP32_GENERIC_S3", variant="SPIRAM_OCT", flash="16MB",
         modules="all", chip="esp32s3", offset=0, kind="esp32", deps="idf", autosize=True),
    # Quad PSRAM (or none found), 8 MB flash: Adafruit QT Py ESP32-S3 (8 MB flash, 2 MB PSRAM) and
    # the other 8 MB boards. 8 MB is the floor that fits the most: an image written with an 8 MB
    # header runs on a board with 16 MB or more, keeping the rest of its flash unused, but a
    # 16 MB image won't boot on an 8 MB board.
    dict(id="mp-s3", label="ESP32-S3 with quad PSRAM (QT Py ESP32-S3, most 8 MB S3 boards)",
         interpreter="micropython", port="esp32", board="ESP32_GENERIC_S3", variant=None, flash="8MB",
         modules="all", chip="esp32s3", offset=0, kind="esp32", deps="idf", autosize=True),
    # No PSRAM, 8 MB flash: an S3 board with only its internal RAM, such as the Adafruit QT Py
    # ESP32-S3 with no PSRAM. Our S3 builds put the TLS library in PSRAM, so this one is the
    # NO_PSRAM variant, which keeps it in internal RAM and Wi-Fi joins a WPA2 network. "all" is
    # left out because LVGL, the camera, the H.264 encoder and the audio instruments want
    # megabytes of heap; this set is what runs in a few hundred KB.
    dict(id="mp-s3-8mb", label="ESP32-S3 with no PSRAM, 8 MB flash (QT Py ESP32-S3 without PSRAM)",
         interpreter="micropython", port="esp32", board="ESP32_GENERIC_S3", variant="NO_PSRAM", flash="8MB",
         modules="pydevices,pygraphics,displayif,pngio,palettes,iconkit,svgio,audiodsp,audioif,mdns",
         chip="esp32s3", offset=0, kind="esp32", deps="idf", autosize=True),
    dict(id="mp-s3-tembed", label="LilyGO T-Embed S3", interpreter="micropython", port="esp32",
         board="ESP32_GENERIC_S3", variant="T_EMBED", flash="16MB", modules="all",
         chip="esp32s3", offset=0, kind="esp32", deps="idf", fs="10880K"),
    dict(id="cp-p4-devkit", label="ESP32-P4-WIFI6-DEV-KIT", interpreter="circuitpython", port="espressif",
         board="waveshare_esp32p4_wifi6_dev_kit", variant=None, flash="16MB",
         modules="audiodsp,pygraphics,pngio,tsmux,ppa,jpegio,cameraif,castif,h264enc,usbhost,pydevices,pdwidgets,palettes,lvgl",
         chip="esp32p4", offset=0, kind="espressif", deps="cp-esp"),
    dict(id="cp-s3-lcd7", label="ESP32-S3-Touch-LCD-7", interpreter="circuitpython", port="espressif",
         board="waveshare_esp32s3_touch_lcd_7", variant=None, flash="8MB",
         modules="audiodsp,pygraphics,pngio,tsmux,pydevices,pdwidgets,palettes,lvgl",
         chip="esp32s3", offset=0, kind="espressif", deps="cp-esp"),
    dict(id="cp-s3-lcd43", label="ESP32-S3-Touch-LCD-4.3", interpreter="circuitpython", port="espressif",
         board="waveshare_esp32s3_touch_lcd_43", variant=None, flash="8MB",
         modules="audiodsp,pygraphics,pngio,tsmux,pydevices,pdwidgets,palettes,lvgl",
         chip="esp32s3", offset=0, kind="espressif", deps="cp-esp"),
    # The Adafruit Qualia ESP32-S3 for RGB666 displays: 16 MB flash, 8 MB octal PSRAM.
    dict(id="cp-s3-qualia", label="Adafruit Qualia ESP32-S3 RGB666", interpreter="circuitpython", port="espressif",
         board="adafruit_qualia_s3_rgb666", variant=None, flash="16MB",
         modules="audiodsp,pygraphics,pngio,tsmux,pydevices,pdwidgets,palettes,lvgl",
         chip="esp32s3", offset=0, kind="espressif", deps="cp-esp"),
    dict(id="cp-twatch", label="LilyGO T-Watch S3", interpreter="circuitpython", port="espressif",
         board="lilygo_twatch_s3_single_app", variant=None, flash="16MB",
         modules="audiodsp,pygraphics,pngio,tsmux,pydevices,pdwidgets,palettes,lvgl",
         chip="esp32s3", offset=0, kind="espressif", deps="cp-esp"),
    dict(id="cp-feather-rp2040", label="Adafruit Feather RP2040 + display wing", interpreter="circuitpython",
         port="raspberrypi", board="feather_rp2040_lvgl", variant=None, flash="8MB",
         modules="pygraphics,pngio,tsmux,pydevices,pdwidgets,palettes,lvgl", chip="rp2040", offset=None, kind="rp2", deps="none"),
    dict(id="mp-giga-shield", label="Arduino GIGA R1 WiFi + GIGA Display Shield", interpreter="micropython",
         port="stm32", board="ARDUINO_GIGA", variant="DISPLAY_SHIELD", flash="2MB",
         modules="displayif,lvgl,audiodsp,audioif,ulab,usbhost", chip="stm32h747", offset=0x08040000, base=0x08000000,
         kind="stm32", deps="none"),
    # Off by default until a board is on the bench to run it on.
    dict(id="mp-pico2", label="Raspberry Pi Pico 2", interpreter="micropython", port="rp2",
         board="RPI_PICO2", variant=None, flash="4MB", modules="all",
         chip="rp2350", offset=None, kind="rp2", deps="none", fs="704K", optional=True),
    dict(id="mp-pico-plus2-w", label="Pimoroni Pico Plus 2 W", interpreter="micropython", port="rp2",
         board="RPI_PICO2_W", variant="PIMORONI_PICO_PLUS2_W", flash="16MB", modules="all",
         chip="rp2350", offset=None, kind="rp2", deps="none", fs="12416K", optional=True),
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
    # `flash` is firmware0.bin, for the microcontroller; `wifi` is firmware1.bin, the Wi-Fi chip's.
    # `romfs` is the image of the firmware's own Python modules, which it doesn't freeze, for the
    # ROMFS partition; `romfs-files` is the same modules as a zip, to add to a ROMFS of your own.
    ("micropython", "stm32"): [("flash", "firmware0.bin", True), ("wifi", "firmware1.bin", True),
                               ("romfs", "romfs.img", True), ("romfs-files", "romfs.zip", True),
                               ("elf", "firmware.elf", True), ("map", "firmware.map", True)],
}

EXTENSION = {"flash": ".bin", "app": ".app.bin", "uf2": ".uf2", "elf": ".elf", "map": ".map",
             "partitions": ".partitions.bin", "wifi": ".bin", "romfs": ".img", "romfs-files": ".zip"}

# The Wi-Fi firmware's address in the GIGA's QSPI flash, and where that flash
# ends. The ROMFS partition is the 4 MB before the Wi-Fi firmware.
WIFI_OFFSET = 0x90F00000
QSPI_END = 0x91000000
ROMFS_OFFSET = 0x90B00000
ROMFS_HEADER = b"\xd2\xcd\x31"


def hex8(n):
    """An address as the flash guide writes it: 0x08040000, 0x90F00000."""
    return "0x%08X" % n


def asset_extension(c, role):
    """The end of a release file's name. A GIGA's two images say which they are
    and where they go, so nobody has to open the notes to tell them apart."""
    if c["kind"] == "stm32" and role == "flash":
        return f".firmware0-at-{hex8(c['offset'])}.bin"
    if c["kind"] == "stm32" and role == "wifi":
        return f".firmware1-at-{hex8(WIFI_OFFSET)}.bin"
    if c["kind"] == "stm32" and role == "romfs":
        return f".romfs-at-{hex8(ROMFS_OFFSET)}.img"
    if c["kind"] == "stm32" and role == "romfs-files":
        return ".romfs-files.zip"
    return EXTENSION[role]


class Failed(Exception):
    pass


def cell(cell_id):
    for c in CELLS:
        if c["id"] == cell_id:
            return c
    raise Failed(f"no cell '{cell_id}' (have: {', '.join(c['id'] for c in CELLS)})")


def wanted(pico2=False):
    return [c for c in CELLS if pico2 or not c.get("optional")]


def size_bytes(text):
    """A cell's `fs` in bytes ("2688K", "1M"), or None if it isn't whole 4 KB flash sectors."""
    m = re.fullmatch(r"(\d+)([KM]?)", text or "")
    n = int(m.group(1)) * {"": 1, "K": 1024, "M": MB}[m.group(2)] if m else 0
    return n if n and n % 4096 == 0 else None


def keeps_fs(c):
    """Whether a cell's filesystem is ours to keep in place: MicroPython's on
    esp32 and rp2, whose layout build_mp.py works out. CircuitPython's boards and
    the GIGA's QSPI filesystem have a layout of their own."""
    return c["interpreter"] == "micropython" and c["kind"] in ("esp32", "rp2")


def check_cells():
    """Every cell whose filesystem build_mp.py places names the size it keeps,
    or says it autosizes."""
    problems = []
    for c in CELLS:
        if c.get("autosize") and (c.get("fs") or c["kind"] != "esp32"):
            problems.append(f"{c['id']}: autosize is for an esp32 cell that names no fs")
        elif keeps_fs(c) and not c.get("autosize") and not size_bytes(c.get("fs")):
            problems.append(f"{c['id']}: names no filesystem to keep (fs={c.get('fs')!r}), so a release "
                            "could move it and empty every board that updates")
        elif not keeps_fs(c) and "fs" in c:
            problems.append(f"{c['id']}: fs is for MicroPython's esp32 and rp2 cells")
    if problems:
        raise Failed("\n".join(problems))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# ---- matrix ----------------------------------------------------------------

def matrix(pico2, only=()):
    """The cells to build: all the wanted ones, or just the ids in `only`."""
    check_cells()
    for cell_id in only:
        cell(cell_id)
    rows = [{k: c[k] for k in ("id", "label", "kind", "deps", "interpreter")}
            for c in wanted(pico2) if not only or c["id"] in only]
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


def build_args(c):
    args = [sys.executable, str(REPO / "build_mp.py"), "--port", c["port"], "--board", c["board"]]
    if c["interpreter"] == "circuitpython":
        args[2:2] = ["--interpreter", "circuitpython"]
    if c["variant"]:
        args += ["--variant", c["variant"]]
    if c["interpreter"] == "micropython" and c["port"] == "esp32":
        args += ["--flash", c["flash"]]
    if c.get("fs"):
        args += ["--keep-fs", c["fs"]]
    args += ["--modules", c["modules"]]
    return args


def build(cell_id, out):
    c = cell(cell_id)
    out = Path(out) / c["id"]
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    args = build_args(c)
    print("+", " ".join(args), flush=True)
    start = time.time()
    rc = subprocess.run(args, cwd=REPO).returncode
    seconds = round(time.time() - start)
    if rc != 0:
        kept = ("" if not c.get("fs") else
                f". If it says the firmware doesn't fit beside the {c['fs']} filesystem, the image has outgrown "
                "the flash this board keeps for it: build fewer modules into it, or give the cell a smaller fs on "
                "purpose, which moves the board's filesystem (the release notes then tell its users to copy their "
                "files off first)")
        raise Failed(f"build_mp.py failed for {c['id']} (exit {rc}) after {seconds} s{kept}")
    src = build_dir(c)
    record = json.loads((src / "pydevices-build.json").read_text())
    if not record.get("complete"):
        raise Failed(f"{src / 'pydevices-build.json'} says the build is not complete")

    if (src / "romfs").is_dir():
        # The modules in romfs.img as files, for anyone who deploys a ROMFS of their own.
        (src / "romfs.zip").unlink(missing_ok=True)
        shutil.make_archive(str(src / "romfs"), "zip", src / "romfs")
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
    if c["kind"] == "stm32":
        meta["flash_offset"], meta["wifi_offset"] = hex8(c["offset"]), hex8(WIFI_OFFSET)
        meta["romfs_offset"], meta["romfs_modules"] = hex8(ROMFS_OFFSET), record.get("romfs_modules") or []
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
    if c["kind"] == "rp2" and record.get("rp2_storage_bytes"):
        # A UF2 doesn't say where the filesystem starts; the build's CMake cache does.
        meta["rp2_storage_bytes"] = record["rp2_storage_bytes"]
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
MIN_BYTES = {"esp32": 1 * MB, "espressif": 1 * MB, "rp2": 512 * 1024, "stm32": 1 * MB}
# Where an RP2040's or RP2350's flash appears, and the UF2 family of a block
# written at an absolute address (the RP2350's errata block past the image).
XIP_BASE = 0x10000000
UF2_FAMILY_ABSOLUTE = 0xE48BFF57


def esp32_fs(table, flash_bytes):
    """(offset, size) of the filesystem MicroPython mounts with this binary
    partition table: the data partition named vfs (or ffat), or else the flash
    after the last partition, which it makes one of at boot."""
    end, named = 0, None
    for at in range(0, len(table) - 31, 32):
        if table[at:at + 2] != b"\xaa\x50":
            break
        ptype, offset, size = table[at + 2], *struct.unpack_from("<II", table, at + 4)
        label = table[at + 12:at + 28].split(b"\0", 1)[0]
        end = max(end, offset + size)
        if ptype == 1 and label in (b"vfs", b"ffat") and named is None:
            named = (offset, size)
    return named or (end, flash_bytes - end)


def uf2_end(blob):
    """The flash address just past the last byte a UF2 writes, leaving out the
    blocks written at an absolute address."""
    end = 0
    for at in range(0, len(blob) - 511, 512):
        flags, addr, size, _, _, family = struct.unpack_from("<IIIIII", blob, at + 8)
        if not (flags & 0x2000 and family == UF2_FAMILY_ABSOLUTE):
            end = max(end, addr + size)
    return end


def check_fs(c, meta, data, flash_bytes):
    """A cell that keeps its filesystem: the build put it where the cell says,
    so a board that updates keeps its files. An autosize cell has no size to keep:
    its filesystem is recorded, not checked. Adds fs_offset and fs_bytes to meta."""
    want = size_bytes(c.get("fs"))
    if c["kind"] == "esp32":
        if "partitions" not in data:
            return []
        offset, size = esp32_fs(data["partitions"].read_bytes(), flash_bytes)
    else:
        size = meta.get("rp2_storage_bytes")
        if not size:
            return [f"{c['id']}: the build recorded no filesystem size, so where it starts isn't known"]
        offset = flash_bytes - size
    meta["fs_offset"], meta["fs_bytes"] = hex(offset), size
    problems = []
    if want is not None and (size != want or offset != flash_bytes - want):
        problems.append(f"{c['id']}: the filesystem is {size // 1024} KB at {offset:#x}, not the {c['fs']} at "
                        f"{flash_bytes - want:#x} the cell keeps: a board that updates would come up with an empty one")
    if "uf2" in data and uf2_end(data["uf2"].read_bytes()) > XIP_BASE + offset:
        problems.append(f"{c['id']}: {data['uf2'].name} writes past {XIP_BASE + offset:#x}, into the filesystem")
    if "flash" in data and c["offset"] + data["flash"].stat().st_size > offset:
        problems.append(f"{c['id']}: {data['flash'].name} writes past {offset:#x}, into the filesystem")
    return problems


def check_stm32(c, data, flash_bytes, modules):
    """What a download would trip over in a GIGA's two images: firmware0.bin has to
    start with a Cortex-M vector table (a stack in RAM, a Thumb reset address
    inside the image) and fit the chip's flash from where it is written, and
    firmware1.bin has to fit the QSPI flash from where it is written. The firmware
    freezes no Python, so its ROMFS image has to be one, fit the partition, and
    hold every module the build moved there, which the zip has to hold too."""
    problems = []
    if "flash" in data:
        img = data["flash"].read_bytes()
        name = data["flash"].name
        if len(img) < MIN_BYTES["stm32"]:
            problems.append(f"{c['id']}: {name} is only {len(img)} bytes")
        start = c["offset"] - c["base"]
        if start + len(img) > flash_bytes:
            problems.append(f"{c['id']}: {name} ends at {c['base'] + start + len(img):#x}, "
                            f"past the {c['flash']} flash")
        if len(img) >= 8:
            stack, reset = struct.unpack("<II", img[:8])
            if not 0x20000000 <= stack <= 0x30000000 or not reset & 1 \
                    or not c["offset"] <= reset < c["offset"] + len(img):
                problems.append(f"{c['id']}: {name} does not start with a vector table "
                                f"(stack {stack:#x}, reset {reset:#x}) for an image written at {c['offset']:#x}")
    if "wifi" in data:
        size = data["wifi"].stat().st_size
        if WIFI_OFFSET + size > QSPI_END:
            problems.append(f"{c['id']}: {data['wifi'].name} ends at {WIFI_OFFSET + size:#x}, past the QSPI flash")
    if not modules:
        problems.append(f"{c['id']}: the build moved no Python modules to ROMFS, so firmware0.bin would boot "
                        "without asyncio, webrepl and the rest")
    if "romfs" in data:
        img = data["romfs"].read_bytes()
        name = data["romfs"].name
        if img[:3] != ROMFS_HEADER:
            problems.append(f"{c['id']}: {name} does not start with a ROMFS header")
        if ROMFS_OFFSET + len(img) > WIFI_OFFSET:
            problems.append(f"{c['id']}: {name} is {len(img)} bytes, more than the ROMFS partition holds "
                            f"from {hex8(ROMFS_OFFSET)} to the Wi-Fi firmware")
        # ROMFS stores each name as it is, so a module's file name is in the image.
        missing = [m for m in modules if m.rsplit("/", 1)[-1].encode() not in img]
        if missing:
            problems.append(f"{c['id']}: {name} lacks {', '.join(missing)}")
    if "romfs-files" in data:
        with zipfile.ZipFile(data["romfs-files"]) as z:
            missing = sorted(set(modules) - set(z.namelist()))
        if missing:
            problems.append(f"{c['id']}: {data['romfs-files'].name} lacks {', '.join(missing)}")
    return problems


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
    if c["kind"] == "stm32":
        problems += check_stm32(c, data, flash_bytes, meta.get("romfs_modules"))
    # A flash image: starts like one, fits the flash, carries the app image.
    elif "flash" in data:
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
    if c.get("fs") or c.get("autosize"):
        problems += check_fs(c, meta, data, flash_bytes)
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


def preview_reasons(upstream_describe, cp_version):
    """Why a set built from these pins is a preview (D3: such a set is never published)."""
    reasons = []
    if "preview" in (upstream_describe or "") or re.search(r"-\d+-g[0-9a-f]+$", upstream_describe or ""):
        reasons.append(f"MicroPython is {upstream_describe}, not a tagged release")
    if cp_version and re.search(r"(alpha|beta|rc)", cp_version):
        reasons.append(f"CircuitPython is {cp_version}, a pre-release")
    return reasons


def assemble(indir, out, pico2, tag, run_url, now=None):
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
            name = base + asset_extension(c, role)
            shutil.copyfile(d / info["file"], out / name)
            files.append({"role": role, "name": name, "size": info["size"], "sha256": info["sha256"]})
        entry = {k: m[k] for k in ("id", "label", "interpreter", "interpreter_version", "board", "variant",
                                   "chip", "flash", "modules", "flash_offset", "build_seconds",
                                   "module_revisions")}
        if m.get("app_offset"):
            entry["app_offset"] = m["app_offset"]
        if m.get("wifi_offset"):
            entry["wifi_offset"] = m["wifi_offset"]
        if m.get("romfs_offset"):
            entry["romfs_offset"], entry["romfs_modules"] = m["romfs_offset"], m["romfs_modules"]
        if "esptool" in m:
            entry["esptool"] = m["esptool"]
        if "circuitpython_patches" in m:
            entry["circuitpython_patches"] = m["circuitpython_patches"]
        if m.get("fs_offset"):
            entry["fs_offset"], entry["fs_bytes"] = m["fs_offset"], m["fs_bytes"]
        entry["files"] = files
        builds.append(entry)

    # Every line of DEPENDENCIES.lock: a checkout's commit (and its tag where
    # the line names one), a download's version.
    pins = {p.name: {"version": p.ref} if p.download else dict({"commit": p.ref}, **({"tag": p.tag} if p.tag else {}))
            for p in pinfile.read(REPO).values()}
    reasons = preview_reasons(mp_version, cp_version)
    doc = {"schema": 2, "tag": tag, "built": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
           "run": run_url, "micropython": mp_version, "circuitpython": cp_version,
           "preview": bool(reasons), "preview_reasons": reasons, "pins": pins, "builds": builds}
    (out / "firmware.json").write_text(json.dumps(doc, indent=1) + "\n")
    return doc


def summary(doc):
    lines = [f"Firmware set {doc['tag']}" + (" (preview pins: never published)" if doc["preview"] else ""),
             "", "| Cell | Build | Flash image | Offset | Filesystem | Build time |", "|---|---|---:|---|---|---:|"]
    for b in doc["builds"]:
        main = next(f for f in b["files"] if f["role"] in ("flash", "uf2"))
        lines.append(f"| {b['id']} | {b['label']} | {main['size'] / MB:.2f} MB | {b.get('flash_offset') or 'UF2'} "
                     f"| {fs_text(b)} | {b['build_seconds'] // 60} min {b['build_seconds'] % 60:02d} s |")
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
    """The line that writes one flash image, or None for a file that isn't one.
    A GIGA's is dfu-util, and its Wi-Fi image goes first (see `notes`)."""
    if b["chip"] == "stm32h747":
        if f["role"] == "wifi":
            return f"dfu-util -a 1 -d 2341:0366 -s {b['wifi_offset']} -D {f['name']}"
        if f["role"] == "romfs":
            return f"dfu-util -a 1 -d 2341:0366 -s {b['romfs_offset']} -D {f['name']}"
        if f["role"] == "flash":
            return f"dfu-util -a 0 -d 2341:0366 -s {b['flash_offset']}:leave -D {f['name']}"
        return None
    if f["role"] != "flash":
        return None
    return f"esptool --chip {b['chip']} write-flash {b['flash_offset']} {f['name']}"


def kind_of(b):
    return "MicroPython" if b["interpreter"] == "micropython" else "CircuitPython-compatible"


def fs_text(b):
    return f"{b['fs_bytes'] // 1024:,} KB at {b['fs_offset']}" if b.get("fs_offset") else ""


def moved(doc, previous):
    """The builds whose filesystem isn't where the previous set put it."""
    before = {b["id"]: b for b in (previous or {}).get("builds", []) if b.get("fs_offset")}
    return [(b, before[b["id"]]) for b in doc["builds"] if b["id"] in before and b.get("fs_offset")
            and (b["fs_offset"], b["fs_bytes"]) != (before[b["id"]]["fs_offset"], before[b["id"]]["fs_bytes"])]


def notes(doc, previous=None):
    """The draft Release's body, for whoever opens it. `previous` is the
    firmware.json of the last set people could download: a board whose
    filesystem this set moves is named at the top."""
    tag = doc["tag"]
    lines = []
    if doc["preview"]:
        lines += ["> **Staged, not for release.** " + "; ".join(doc["preview_reasons"]) +
                  ". This draft stays private until a set is built from tagged versions.", ""]
    lines += [f"OmniPython firmware {tag}: MicroPython {doc['micropython']}"
              + (f" and CircuitPython-compatible {doc['circuitpython']}" if doc.get("circuitpython") else "")
              + f", built by [this run]({doc['run']}) on {doc['built'][:10]}.", ""]
    shifted = moved(doc, previous)
    if shifted:
        lines += [f"**This release moves the filesystem** on the boards below. Updating one from {previous['tag']} "
                  "leaves it with an empty filesystem, so copy its files off first, and put them back after.", ""]
        lines += [f"- {b['label']}, {kind_of(b)}: {fs_text(b)}, was {fs_text(old)}" for b, old in shifted]
        lines += [""]
    lines += ["**Before you flash,** copy your files off the board, `secrets.py` above all. Each MicroPython "
              "board's filesystem keeps its size and place from one release to the next, as the table gives it, "
              "and a release that moves one says so at the top. After a flash across a layout change MicroPython "
              "finds the old data where its filesystem now starts, says it looks corrupted and mounts nothing. "
              "So when the filesystem moves, or you come from another firmware (MicroPython to CircuitPython or "
              "back), run `esptool --chip <chip> erase-flash` first, then put your files back.", "",
              "| Board | Interpreter | File to flash | How | Filesystem |", "|---|---|---|---|---|"]
    for b in doc["builds"]:
        f = next(x for x in b["files"] if x["role"] in ("flash", "uf2"))
        how = ("copy it onto the board's BOOTSEL drive" if f["role"] == "uf2"
               else f"`{write_command(b, f)}`")
        names = f"`{f['name']}`"
        before = [x for role in ("wifi", "romfs") for x in b["files"] if x["role"] == role]
        if before:
            # The GIGA's images, the Wi-Fi firmware first and firmware0 last.
            names = ", then ".join(f"`{x['name']}`" for x in before + [f])
            how = ", then ".join(f"`{write_command(b, x)}`" for x in before + [f])
        lines.append(f"| {b['label']} | {kind_of(b)} {b['interpreter_version']} | {names} | {how} | {fs_text(b)} |")
    if any(b["chip"] == "stm32h747" for b in doc["builds"]):
        lines += ["",
                  "The GIGA's three images are written in its bootloader (`machine.bootloader()` at the REPL, or "
                  "double-tap RESET), the Wi-Fi firmware first. The Wi-Fi image only changes when its driver does, "
                  "so after the first time you can leave it out. The ROMFS image holds the firmware's own Python "
                  "modules (asyncio, webrepl, mip, requests and the rest), which it doesn't freeze, so write it with "
                  "every update. The last image overwrites any program on the board's M4 core. To add the "
                  "OmniPython libraries to the ROMFS, unzip the `romfs-files` zip into the folder you deploy, "
                  "because a deploy replaces the whole partition: "
                  f"[the flash guide](https://github.com/{REPO_SLUG}/blob/main/docs/guides/flash-a-board.md"
                  "#stm32-arduino-giga-r1-wifi) has the steps."]
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
    m.add_argument("--only", default="", help="comma list of cell ids to build instead of the whole set")
    b = sub.add_parser("build")
    b.add_argument("cell")
    b.add_argument("out")
    a = sub.add_parser("assemble")
    a.add_argument("indir")
    a.add_argument("out")
    a.add_argument("--pico2", action="store_true")
    a.add_argument("--tag", default="")
    a.add_argument("--run-url", default="")
    n = sub.add_parser("notes")
    n.add_argument("firmware_json")
    n.add_argument("--previous", help="the firmware.json of the last set people could download")
    n.add_argument("--out")
    sub.add_parser("cells")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "matrix":
            print(json.dumps(matrix(args.pico2, [i for i in args.only.split(",") if i]), separators=(",", ":")))
        elif args.cmd == "cells":
            for c in CELLS:
                print(f"{c['id']:20} {c['interpreter']:14} {c['board']} {c['variant'] or ''}"
                      f"{'  (off by default)' if c.get('optional') else ''}")
        elif args.cmd == "notes":
            doc = json.loads(Path(args.firmware_json).read_text())
            check_draft_tag(doc)
            previous = json.loads(Path(args.previous).read_text()) if args.previous else None
            text = notes(doc, previous)
            for b, old in moved(doc, previous):
                print(f"note: {b['id']}'s filesystem moves: {fs_text(b)}, was {fs_text(old)} in {previous['tag']}",
                      file=sys.stderr)
            if args.out:
                Path(args.out).write_text(text)
            else:
                print(text)
        elif args.cmd == "build":
            build(args.cell, args.out)
        elif args.cmd == "assemble":
            doc = assemble(args.indir, args.out, args.pico2, args.tag, args.run_url)
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
