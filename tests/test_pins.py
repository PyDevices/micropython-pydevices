# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""scripts/pins.py: DEPENDENCIES.lock, read the one way every script and workflow reads it."""

import subprocess
import sys
import unittest
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
        got = pins.parse("micropython u %s\nomnipython-lvgl u %s\nesp-idf u %s\naudiodsp u %s\n" % (C, C, C, C))
        self.assertEqual({p.name: p.group for p in got.values()},
                         {"micropython": "micropython", "omnipython-lvgl": "sources",
                          "esp-idf": "toolchains", "audiodsp": "modules"})


class TheFile(unittest.TestCase):
    """The committed DEPENDENCIES.lock, as the build reads it."""

    def setUp(self):
        self.lock = pins.read(HERE)

    def test_it_parses_and_pins_micropython(self):
        self.assertEqual(self.lock["micropython"].group, "micropython")

    def test_every_toolchain_and_source_is_pinned(self):
        for name in pins.TOOLCHAINS + pins.SOURCES:
            self.assertIn(name, self.lock)

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


if __name__ == "__main__":
    unittest.main()
