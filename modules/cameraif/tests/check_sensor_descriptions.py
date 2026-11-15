# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Check the sensor descriptions in sensors/ without a camera.

Without arguments it needs only CPython. It loads every description and checks
what Camera() would refuse at run time: the keys the table engine can't do
without, the packed register lists (three bytes an entry), the Bayer and
format names, and that each file says which esp_cam_sensor version it came from
and that it is the version DEPENDENCIES.lock pins.

With --download (or --sensors, an unpacked esp_cam_sensor) it also runs
Espressif's own driver for each sensor on the desktop with gcc, and checks two
ways that the description is what the driver does: tools/sensorgen/sensorgen.py
--check regenerates each description and compares it with the file, and
tools/sensorgen/replay_check.py plays each one back against the driver,
register for register, for every mode, the stream, the flips, the test pattern
and every setting of exposure, gain and the other controls. A hand edit to a
description, or a change of the pinned driver, fails both.

    python tests/check_sensor_descriptions.py
    python tests/check_sensor_descriptions.py --download
    python tests/check_sensor_descriptions.py --sensors path/to/esp_cam_sensor
"""
import argparse
import importlib.util
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check_sensor_configs as configs  # noqa: E402

MODULE = configs.MODULE
SENSORS = configs.SENSORS
TOOLS = MODULE / "tools" / "sensorgen"

BAYER = {"RGGB", "GRBG", "GBRG", "BGGR"}
FORMATS = {"raw8", "raw10", "raw12"}
MODE_INTS = ("width", "height", "lane_hz", "vts")


def load(path):
    spec = importlib.util.spec_from_file_location(path.stem, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.SENSOR


def packed(regs, where, problems):
    """A register list is packed: three bytes an entry."""
    if isinstance(regs, (bytes, bytearray)):
        if len(regs) % 3:
            problems.append("%s: packed registers are three bytes each, this is %d long" % (where, len(regs)))
    elif not isinstance(regs, (tuple, list)):
        problems.append("%s: registers are packed bytes or (register, value) pairs" % where)


def check_description(path, version):
    """The problems with one description, as a list of strings."""
    name = path.stem
    problems = []

    def bad(text):
        problems.append("sensors/%s.py: %s" % (name, text))

    try:
        desc = load(path)
    except Exception as e:
        return ["sensors/%s.py does not import: %s: %s" % (name, type(e).__name__, e)]
    if not isinstance(desc, dict):
        return ["sensors/%s.py: SENSOR is not a dict" % name]
    for key in ("addr", "id", "modes"):
        if desc.get(key) is None:
            bad("needs '%s'" % key)
    if desc.get("name") is not None and not isinstance(desc["name"], str):
        bad("name is not a string")
    ident = desc.get("id")
    if ident is not None and (not isinstance(ident, tuple) or len(ident) != 2):
        bad("id is (register, value)")
    elif ident is not None and not isinstance(ident[0], int) and not 1 <= len(ident[0]) <= 4:
        bad("id reads one to four registers")
    if desc.get("reg_bits", 16) not in (8, 16):
        bad("reg_bits is 8 or 16")
    for key in ("init", "id_pre"):
        if desc.get(key) is not None:
            packed(desc[key], "sensors/%s.py %s" % (name, key), problems)
    modes = desc.get("modes") or ()
    if not modes:
        bad("needs at least one mode")
    seen = set()
    for k, mode in enumerate(modes):
        where = "sensors/%s.py mode %d" % (name, k)
        for key in MODE_INTS:
            if not isinstance(mode.get(key), int) or mode[key] <= 0:
                problems.append("%s: '%s' is a positive number" % (where, key))
        if mode.get("bayer") not in BAYER:
            problems.append("%s: bayer is one of RGGB, GRBG, GBRG, BGGR" % where)
        if mode.get("format") not in FORMATS:
            problems.append("%s: format is raw8, raw10 or raw12" % where)
        if mode.get("regs") is None:
            problems.append("%s: needs 'regs'" % where)
        else:
            packed(mode["regs"], where, problems)
        if mode.get("stream") is not None:
            for part in mode["stream"]:
                packed(part, where + " stream", problems)
        label = mode.get("name")
        if label in seen:
            problems.append("%s: its name repeats an earlier mode's" % where)
        seen.add(label)
    text = path.read_text()
    m = re.search(r"Generated from espressif/esp_cam_sensor ([\d.]+)", text)
    if not m:
        bad("its header doesn't say which esp_cam_sensor version it was generated from")
    elif m.group(1) != version:
        bad("generated from esp_cam_sensor %s, but DEPENDENCIES.lock pins %s" % (m.group(1), version))
    return problems


def pinned_version():
    for line in configs.LOCK.read_text().splitlines():
        parts = line.split()
        if parts and parts[0] == "esp_cam_sensor":
            return parts[2]
    sys.exit("DEPENDENCIES.lock has no esp_cam_sensor line")


def run(label, cmd):
    """Run a tool, show what it said, and return its problems."""
    done = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    out = done.stdout + done.stderr
    print(out, end="" if out.endswith("\n") else "\n")
    if done.returncode:
        return ["%s failed (exit %d)" % (label, done.returncode)]
    return []


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sensors", type=Path, help="an unpacked esp_cam_sensor")
    ap.add_argument("--download", action="store_true", help="fetch the pinned esp_cam_sensor")
    args = ap.parse_args()

    version = pinned_version()
    paths = sorted(SENSORS.glob("*.py"))
    problems = []
    for path in paths:
        problems += check_description(path, version)
    print("%d descriptions read" % len(paths))

    source = args.sensors or (configs.pinned_sensors() if args.download else None)
    if source is not None:
        names = [p.stem for p in paths]
        problems += run("sensorgen --check", [sys.executable, str(TOOLS / "sensorgen.py"),
                                              "--ecs", str(source), "--check"] + names)
        problems += run("replay_check", [sys.executable, str(TOOLS / "replay_check.py"),
                                         "--ecs", str(source)] + names)

    for p in problems:
        print(p)
    print("%s" % ("in step" if not problems else "%d problems" % len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
