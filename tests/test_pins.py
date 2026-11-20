# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""scripts/pins.py: DEPENDENCIES.lock, read the one way every script and workflow reads it."""

import os
import subprocess
import sys
import unittest
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE / "scripts"))
import pins  # noqa: E402

C = "0123456789abcdef0123456789abcdef01234567"


class Parse(unittest.TestCase):
    def test_a_commit_line_with_and_without_a_tag(self):
        got = pins.parse("# a comment\n\nmod https://x/mod.git %s\nidf https://x/idf.git %s  # tag v5.5.4\n" % (C, C))
        self.assertEqual(got["mod"], pins.Pin("mod", "https://x/mod.git", C, ""))
        self.assertEqual(got["idf"].tag, "v5.5.4")

    def test_a_comment_after_a_line_is_not_a_tag(self):
        self.assertEqual(pins.parse("mod https://x/mod.git %s  # the old one\n" % C)["mod"].tag, "")

    def test_a_download_takes_a_version(self):
        p = pins.parse("SDL2 https://x/SDL2-2.30.10.tar.gz 2.30.10\n")["SDL2"]
        self.assertTrue(p.download)
        self.assertEqual(p.ref, "2.30.10")

    def test_a_checkout_takes_a_full_commit(self):
        for ref in ("84d1de3", "v5.5.4", C[:-1] + "g", C + "0"):
            with self.assertRaisesRegex(pins.LockError, "full 40-character commit"):
                pins.parse("mod https://x/mod.git %s\n" % ref)

    def test_a_line_needs_three_fields_and_a_name_once(self):
        with self.assertRaisesRegex(pins.LockError, ":1: want"):
            pins.parse("mod https://x/mod.git\n")
        with self.assertRaisesRegex(pins.LockError, ":2: mod is pinned twice"):
            pins.parse("mod https://x/a.git %s\nmod https://x/b.git %s\n" % (C, C))

    def test_groups(self):
        got = pins.parse("micropython u %s\nomnipython-lvgl u %s\nesp-idf u %s\naudiodsp u %s\nesp-idf6 u %s\n"
                         % (C, C, C, C, C))
        self.assertEqual({p.name: p.group for p in got.values()},
                         {"micropython": "micropython", "omnipython-lvgl": "sources",
                          "esp-idf": "toolchains", "audiodsp": "modules", "esp-idf6": "s31"})


class TheFile(unittest.TestCase):
    """The committed DEPENDENCIES.lock, as the build reads it."""

    def setUp(self):
        self.lock = pins.read(HERE)

    def test_it_parses_and_pins_micropython(self):
        self.assertEqual(self.lock["micropython"].group, "micropython")

    def test_every_toolchain_and_source_is_pinned(self):
        for name in pins.TOOLCHAINS + pins.SOURCES:
            self.assertIn(name, self.lock)

    def test_the_s31_pins_are_their_own_group(self):
        # An S31 line in another group would reach every build: a module is
        # fetched by every build, and a toolchain moves the cache key.
        for name in pins.S31:
            self.assertEqual(self.lock[name].group, "s31", name)
        self.assertTrue(self.lock["esp-idf6"].tag)

    def test_the_toolchains_fetched_by_tag_name_their_tag(self):
        for name in ("esp-idf", "emsdk", "espressif__tinyusb", "circuitpython"):
            self.assertTrue(self.lock[name].tag, name)

    def test_build_mp_fetches_no_component_this_file_does_not_name_a_toolchain(self):
        sys.path.insert(0, str(HERE))
        import build_mp
        self.assertLessEqual(build_mp.CP_ESP_IDF_COMPONENT_DEPS, set(pins.TOOLCHAINS))

    def test_the_command_line(self):
        def run(*a):
            return subprocess.run([sys.executable, str(HERE / "scripts" / "pins.py"), *a],
                                  capture_output=True, text=True)
        self.assertEqual(run("micropython").stdout.strip(), self.lock["micropython"].ref)
        self.assertEqual(run("circuitpython", "tag").stdout.strip(), self.lock["circuitpython"].tag)
        self.assertEqual(run("esp-vision", "url").stdout.strip(), self.lock["esp-vision"].url)
        self.assertEqual(len(run("--key", "toolchains").stdout.strip()), 16)
        self.assertNotEqual(run("SDL2", "commit").returncode, 0)
        self.assertNotEqual(run("no-such-pin").returncode, 0)


class FrozenLibrary(unittest.TestCase):
    """What the pinned library freezes, read from omnipython on GitHub.

    A frozen package hides one of the same name in /lib, so a module the
    library's frozen packages lack can't be added to a board with mip. Offline
    the checks skip; OMNIPYTHON_REQUIRE_NETWORK=1 makes that a failure.
    """

    def has(self, path):
        pin = pins.read(HERE)["omnipython-lib"]
        repo = pin.url.removeprefix("https://github.com/").removesuffix(".git")
        url = "https://raw.githubusercontent.com/%s/%s/%s" % (repo, pin.ref, path)
        try:
            with urllib.request.urlopen(urllib.request.Request(url, method="HEAD"), timeout=20):
                return True
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return False
            raise
        except urllib.error.URLError as e:
            if os.environ.get("OMNIPYTHON_REQUIRE_NETWORK") == "1":
                raise
            self.skipTest("no network to read %s: %s" % (url, e.reason))

    def test_the_frozen_usb_package_reads_gamepads(self):
        # The firmware's usb package carries usb.host.hid_generic, the gamepad
        # and joystick reader: a copy in /lib/usb would never be imported.
        self.assertTrue(self.has("packages/usb/usb-host-hid/usb/host/hid_generic.py"),
                        "the pinned omnipython-lib has no usb.host.hid_generic")


if __name__ == "__main__":
    unittest.main()
