# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Check that every ESP32-P4 build leaves the sensor drivers out and describes every sensor.

cameraif's cameras are descriptions in sensors/, frozen into every P4 image
(cameraif_sensors.NAMES). So no esp_cam_sensor driver is built in: the fragment
build_mp.py appends to every P4 build (sdkconfig.esp32p4) and the patch that
gives a MicroPython tree outside build_mp.py the same settings (patches/0002)
set no CONFIG_CAMERA_* option, and the CircuitPython board, which has no
cameraif, is not looked at.

Given esp_cam_sensor's source it also checks that every driver with a MIPI-CSI
interface has a description in sensors/ and is in cameraif_sensors.NAMES, or is
one of the few that can't be described, and that every description names a
driver. With --download it fetches the version DEPENDENCIES.lock pins.

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
SENSORS = MODULE / "sensors"
FROZEN = MODULE / "frozen" / "cameraif_sensors.py"
LOCK = REPO / "DEPENDENCIES.lock"

SETTING = re.compile(r"^(CONFIG_CAMERA_\w+)=(\S+)$")

# MIPI drivers with no description, and why: a description is data, and these
# are not.
NOT_DESCRIBED = {
    "arducam_imx500": "polls its module's controller and reads its modes at run time",
    "pivariety": "polls its module's controller and reads its modes at run time",
    "lt6911": "an HDMI-to-CSI bridge whose mode follows the HDMI source",
    "tc358743": "an HDMI-to-CSI bridge whose mode follows the HDMI source",
    "gc2145": "no Bayer mode: cameraif's ISP takes only RAW",
    "ov5640": "no Bayer mode: cameraif's ISP takes only RAW",
    "ov5645": "no Bayer mode: cameraif's ISP takes only RAW",
    "sc121at": "no Bayer mode: cameraif's ISP takes only RAW",
}


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
    """The names of the esp_cam_sensor drivers with a MIPI-CSI interface."""
    names = set()
    for kconfig in sorted((source / "sensors").glob("**/Kconfig.*")):
        top = None
        for line in kconfig.read_text().splitlines():
            m = re.match(r"\s*(?:menu)?config (CAMERA_\w+)$", line)
            if not m:
                continue
            if top is None:
                top = m.group(1)
            elif m.group(1).endswith("_AUTO_DETECT_MIPI_INTERFACE_SENSOR"):
                names.add(top[len("CAMERA_"):].lower())
    return names


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


def frozen_names():
    """The names cameraif_sensors.NAMES lists."""
    m = re.search(r"NAMES = \((.*?)\)", FROZEN.read_text(), re.S)
    return re.findall(r'"(\w+)"', m.group(1)) if m else []


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sensors", type=Path, help="an unpacked esp_cam_sensor")
    ap.add_argument("--download", action="store_true", help="fetch the pinned esp_cam_sensor")
    args = ap.parse_args()

    problems = []
    for name, found in (("sdkconfig.esp32p4", settings(FRAGMENT.read_text().splitlines())),
                        ("patches/0002", settings(patch_lines(PATCH)))):
        for option in sorted(found):
            problems.append("%s sets %s: no sensor driver is built in, the descriptions are frozen" % (name, option))

    described = sorted(p.stem for p in SENSORS.glob("*.py"))
    listed = frozen_names()
    for name in described:
        if name not in listed:
            problems.append("sensors/%s.py is not in cameraif_sensors.NAMES" % name)
    for name in listed:
        if name not in described:
            problems.append("cameraif_sensors.NAMES lists %s, which has no description" % name)
    if len(set(listed)) != len(listed):
        problems.append("cameraif_sensors.NAMES lists a sensor twice")

    source = args.sensors or (pinned_sensors() if args.download else None)
    if source is not None:
        drivers = mipi_drivers(source)
        for name in sorted(drivers):
            if name not in described and name not in NOT_DESCRIBED:
                problems.append("%s has a MIPI-CSI driver and no description in sensors/" % name)
        for name in described:
            if name not in drivers:
                problems.append("sensors/%s.py names no driver in this esp_cam_sensor" % name)
        for name in sorted(NOT_DESCRIBED):
            if name in described:
                problems.append("%s is described but listed as one that can't be" % name)
        print("%d MIPI-CSI drivers in %s" % (len(drivers), source))

    for p in problems:
        print(p)
    print("%d sensors described, %s" % (len(described), "in step" if not problems else "%d problems" % len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
