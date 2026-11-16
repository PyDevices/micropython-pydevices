# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""usb.device, with our micropython-lib patches, refuses to start a device
while the USB host runs on an ESP32-S2 or S3, whose one port they share.

The test takes usb/device/core.py from the micropython-lib checkout under
micropython/ (or $MICROPYTHON), applies patches/micropython-lib/ to a copy,
and runs it on CPython over a fake machine.USBDevice and _usbhost. It skips
without a checkout, unless OMNIPYTHON_REQUIRE_MICROPYTHON is set, as CI does.

    PYTHONPATH=tests python3 -m unittest test_usb_device_port -v
"""

import importlib.util
import os
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
MICROPYTHON = Path(os.environ.get("MICROPYTHON", HERE / "micropython"))
LIB = MICROPYTHON / "lib" / "micropython-lib"
CORE = "micropython/usb/usb-device/usb/device/core.py"


def _patched_core(tmp):
    """core.py from the checkout's commit, with our patches applied."""
    commit = subprocess.run(["git", "-C", str(LIB), "rev-parse", "HEAD"], capture_output=True, text=True,
                            check=True).stdout.strip()
    work = Path(tmp) / "mpl"
    subprocess.run(["git", "init", "-q", str(work)], check=True)
    for path in (CORE, "micropython/usb/usb-device/manifest.py"):
        text = subprocess.run(["git", "-C", str(LIB), "show", "%s:%s" % (commit, path)], capture_output=True,
                              text=True, check=True).stdout
        (work / path).parent.mkdir(parents=True, exist_ok=True)
        (work / path).write_text(text)
    patches = sorted((HERE / "patches" / "micropython-lib").glob("*.patch"))
    for patch in patches:
        # Only the patches that touch usb-device apply to this copy.
        if "micropython/usb/usb-device/" in patch.read_text():
            subprocess.run(["git", "-C", str(work), "apply", "--include=micropython/usb/usb-device/*",
                            str(patch)], check=True)
    return work / CORE


class _USBDevice:
    BUILTIN_NONE = types.SimpleNamespace(desc_cfg=b"")
    BUILTIN_DEFAULT = BUILTIN_NONE

    def __init__(self):
        self.on = True
        self.switches = []

    def active(self, *value):
        if value:
            self.switches.append(bool(value[0]))
            self.on = bool(value[0])
        return self.on


class SharedPort(unittest.TestCase):
    def setUp(self):
        if not (LIB / CORE).is_file():
            why = "needs a micropython-lib checkout under micropython/ or $MICROPYTHON"
            if os.environ.get("OMNIPYTHON_REQUIRE_MICROPYTHON"):
                self.fail(why)
            self.skipTest(why)
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        source = _patched_core(tmp.name)
        self.host = [True]
        micropython = types.ModuleType("micropython")
        micropython.const = lambda v: v
        machine = types.ModuleType("machine")
        machine.USBDevice = _USBDevice
        usbhost = types.ModuleType("_usbhost")
        usbhost.host_stats = lambda: (1 if self.host[0] else 0, 0, 0, 0, 0)
        fakes = {"micropython": micropython, "machine": machine, "_usbhost": usbhost}
        saved = {k: sys.modules.get(k) for k in fakes}
        sys.modules.update(fakes)

        def restore():
            for key, value in saved.items():
                if value is None:
                    sys.modules.pop(key, None)
                else:
                    sys.modules[key] = value

        self.addCleanup(restore)
        spec = importlib.util.spec_from_file_location("usb_device_core_under_test", source)
        self.core = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.core)
        self.machine_name = getattr(sys.implementation, "_machine", None)
        self.addCleanup(self._restore_machine)

    def _restore_machine(self):
        if self.machine_name is None:
            if hasattr(sys.implementation, "_machine"):
                del sys.implementation._machine
        else:
            sys.implementation._machine = self.machine_name

    def on(self, machine):
        sys.implementation._machine = machine
        return self.core._Device()

    def test_an_s3_refuses_while_the_host_runs_and_leaves_the_device_alone(self):
        dev = self.on("Generic ESP32S3 module with ESP32S3")
        with self.assertRaises(OSError) as caught:
            dev.init(builtin_driver=True)
        self.assertIn("usb.core.stop()", str(caught.exception))
        self.assertEqual(dev._usbd.switches, [])
        with self.assertRaises(OSError):
            dev.active(True)
        self.assertEqual(dev._usbd.switches, [])

    def test_an_s2_refuses_too(self):
        dev = self.on("ESP32-S2-DevKitC with ESP32S2")
        with self.assertRaises(OSError):
            dev.active(True)

    def test_an_s3_starts_once_the_host_stops(self):
        dev = self.on("Generic ESP32S3 module with ESP32S3")
        self.host[0] = False
        dev.active(True)
        self.assertEqual(dev._usbd.switches, [True])

    def test_a_p4_keeps_its_host_and_device_apart(self):
        # The P4's host port is its own controller.
        dev = self.on("Waveshare ESP32-P4-WIFI6-DEV-KIT with ESP32P4")
        dev.active(True)
        self.assertEqual(dev._usbd.switches, [True])

    def test_switching_off_is_always_allowed(self):
        dev = self.on("Generic ESP32S3 module with ESP32S3")
        dev.active(False)
        self.assertEqual(dev._usbd.switches, [False])


if __name__ == "__main__":
    unittest.main()
