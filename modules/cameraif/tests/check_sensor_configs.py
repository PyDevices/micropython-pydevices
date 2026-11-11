# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Check that every ESP32-P4 build turns on the same camera sensors.

Three files carry cameraif's sensor settings: the fragment build_mp.py appends
to every P4 build (sdkconfig.esp32p4), the patch that gives a MicroPython tree
outside build_mp.py the same settings (patches/0002), and the CircuitPython
board's sdkconfig. This checks that all three set the same CONFIG_CAMERA_*
options to the same values.

Given esp_cam_sensor's source, it also checks that every driver with a MIPI-CSI
interface is on (but those whose description is frozen in instead), with its MIPI auto-detect and every MIPI mode it has, and no
DVP or SPI auto-detect, since cameraif only drives the CSI port. With
--download it fetches the version DEPENDENCIES.lock pins.

    python tests/check_sensor_configs.py
    python tests/check_sensor_configs.py --download
    python tests/check_sensor_configs.py --sensors path/to/esp_cam_sensor
"""
import argparse
import io
import re
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

MODULE = Path(__file__).resolve().parent.parent
REPO = MODULE.parent.parent
FRAGMENT = MODULE / "sdkconfig.esp32p4"
PATCH = MODULE / "patches" / "0002-micropython-esp32-p4-camera-sensor-sdkconfig.patch"
CP_SDKCONFIG = REPO / "boards" / "circuitpython" / "espressif" / "waveshare_esp32p4_wifi6_dev_kit" / "sdkconfig"
LOCK = REPO / "DEPENDENCIES.lock"

SETTING = re.compile(r"^(CONFIG_CAMERA_\w+)=(\S+)$")

# Sensors whose C driver the MicroPython images leave out: their descriptions
# in sensors/ are frozen in instead. The CircuitPython board has no cameraif and
# keeps the driver.
DESCRIBED = ("OV2710", "OV5647")


def is_described(option):
    """Whether a CONFIG_CAMERA_* option belongs to a sensor frozen in as a description."""
    return any(option.startswith("CONFIG_CAMERA_%s" % name) for name in DESCRIBED)


def settings(lines):
    """{option: value} for the CONFIG_CAMERA_* lines among *lines*."""
    found = {}
    for line in lines:
        m = SETTING.match(line.strip())
        if m:
            found[m.group(1)] = m.group(2)
    return found


def patch_lines(patch):
    """The lines patch 0002 adds to boards/sdkconfig.p4_camera."""
    lines, inside = [], False
    for line in patch.read_text().splitlines():
        if line.startswith("diff --git"):
            inside = line.endswith("/sdkconfig.p4_camera")
        elif inside and line.startswith("+") and not line.startswith("+++"):
            lines.append(line[1:])
    return lines


def mipi_drivers(source):
    """{driver option: (MIPI auto-detect options, other auto-detect options,
    MIPI mode options)} for every esp_cam_sensor driver with a MIPI-CSI port."""
    drivers = {}
    for kconfig in sorted((source / "sensors").glob("**/Kconfig.*")):
        top, names, choice = None, [], False
        for line in kconfig.read_text().splitlines():
            s = line.strip()
            if s.startswith("choice"):
                choice = True
            elif s.startswith("endchoice"):
                choice = False
            m = re.match(r"(?:menu)?config (CAMERA_\w+)$", s)
            if not m:
                continue
            if top is None:
                top = m.group(1)
            elif not choice:
                names.append(m.group(1))
        mipi_detect = [n for n in names if n.endswith("_AUTO_DETECT_MIPI_INTERFACE_SENSOR")]
        if not mipi_detect:
            continue
        other_detect = [n for n in names if re.search(r"_AUTO_DETECT_(DVP|SPI)_INTERFACE_SENSOR$", n)]
        modes = [n for n in names if re.search(r"_MIPI_(RAW|RGB|YUV)\w*\d+FPS", n)]
        drivers[top] = (mipi_detect, other_detect, modes)
    return drivers


def pinned_sensors():
    """esp_cam_sensor at the version DEPENDENCIES.lock pins, unpacked in a
    temporary directory."""
    for line in LOCK.read_text().splitlines():
        parts = line.split()
        if parts and parts[0] == "esp_cam_sensor":
            url = parts[1]
            break
    else:
        sys.exit("DEPENDENCIES.lock has no esp_cam_sensor line")
    tmp = Path(tempfile.mkdtemp(prefix="esp_cam_sensor-"))
    with urllib.request.urlopen(url) as r:
        zipfile.ZipFile(io.BytesIO(r.read())).extractall(tmp)
    return tmp


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sensors", type=Path, help="an unpacked esp_cam_sensor")
    ap.add_argument("--download", action="store_true", help="fetch the pinned esp_cam_sensor")
    args = ap.parse_args()

    problems = []
    fragment = settings(FRAGMENT.read_text().splitlines())
    for option in sorted(fragment):
        if is_described(option):
            problems.append("%s is set in sdkconfig.esp32p4, but that sensor is frozen in as a description" % option)
    for name, other in (("patches/0002", settings(patch_lines(PATCH))),
                        ("the CircuitPython board's sdkconfig", settings(CP_SDKCONFIG.read_text().splitlines()))):
        for option in sorted(set(fragment) | set(other)):
            if name != "patches/0002" and is_described(option):
                continue
            if fragment.get(option) != other.get(option):
                problems.append("%s: %s is %s in sdkconfig.esp32p4, %s in %s"
                                % (option, option, fragment.get(option, "unset"), other.get(option, "unset"), name))

    source = args.sensors or (pinned_sensors() if args.download else None)
    if source is not None:
        drivers = mipi_drivers(source)
        for top, (mipi_detect, other_detect, modes) in sorted(drivers.items()):
            if is_described("CONFIG_" + top):
                continue
            for option in [top] + mipi_detect + modes:
                if fragment.get("CONFIG_" + option) != "y":
                    problems.append("CONFIG_%s isn't on: every MIPI-CSI driver is, with every mode" % option)
            for option in other_detect:
                if fragment.get("CONFIG_" + option) == "y":
                    problems.append("CONFIG_%s is on: cameraif only drives the CSI port" % option)
        print("%d MIPI-CSI drivers in %s" % (len(drivers), source))

    for p in problems:
        print(p)
    print("%d CONFIG_CAMERA_* settings, %s" % (len(fragment), "in step" if not problems else "%d problems" % len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
