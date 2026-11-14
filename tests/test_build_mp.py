# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Unit tests for build_mp.py's decisions that need no build.

    PYTHONPATH=tests python3 -m unittest test_build_mp -v
"""

import contextlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / "scripts"))
import build_mp  # noqa: E402
import release_firmware as rf  # noqa: E402

A = "a" * 40
B = "b" * 40


def row(commit=A, dirty=False, changes=None):
    return {"path": "/x", "revision": commit[:12], "commit": commit, "dirty": dirty, "changes": changes}


class OffPin(unittest.TestCase):
    """A module checkout that isn't at its DEPENDENCIES.lock pin is named, with which way it's off."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.repo = Path(self.dir.name)
        git = ["git", "-C", str(self.repo), "-c", "user.name=t", "-c", "user.email=t@t"]
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        self.commits = []
        for i in range(3):
            subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", str(i)], check=True)
            self.commits.append(subprocess.run(git + ["rev-parse", "HEAD"], capture_output=True,
                                               text=True, check=True).stdout.strip())
        self.lock = build_mp.lock

    def tearDown(self):
        build_mp.lock = self.lock
        self.dir.cleanup()

    def off(self, pin, commit):
        build_mp.lock = lambda: {"audiodsp": build_mp.pins.Pin("audiodsp", "https://example.invalid/audiodsp.git", pin)}
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            got = build_mp.off_pin("audiodsp", self.repo, commit)
        return got, out.getvalue()

    def test_at_pin_says_nothing(self):
        self.assertEqual(self.off(self.commits[1], self.commits[1]), (None, ""))

    def test_behind_pin_is_named_with_how_to_catch_up(self):
        got, said = self.off(self.commits[2], self.commits[1])
        self.assertEqual(got, "behind " + self.commits[2][:12])
        self.assertIn("audiodsp", said)
        self.assertIn("pull", said)

    def test_ahead_of_pin(self):
        self.assertEqual(self.off(self.commits[0], self.commits[2])[0], "ahead of " + self.commits[0][:12])

    def test_pin_not_fetched(self):
        self.assertTrue(self.off(B, self.commits[2])[0].startswith("not at"))

    def test_module_without_a_lock_row(self):
        build_mp.lock = lambda: {}
        self.assertIsNone(build_mp.off_pin("pydevices", self.repo, self.commits[0]))


class Esp32Speed(unittest.TestCase):
    """A build that leaves the CPU below its chip's rating, or flash or PSRAM
    below 80 MHz, says so after it."""

    def note(self, sdk):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            build_mp.esp32_speed_note(sdk)
        return out.getvalue()

    def test_40mhz_flash_and_psram_are_named(self):
        sdk = "CONFIG_ESPTOOLPY_FLASHFREQ_40M=y\nCONFIG_SPIRAM=y\nCONFIG_SPIRAM_SPEED_40M=y\n"
        self.assertIn("flash at 40 MHz and PSRAM at 40 MHz", self.note(sdk))

    def test_80mhz_is_quiet(self):
        sdk = "CONFIG_ESPTOOLPY_FLASHFREQ_80M=y\nCONFIG_SPIRAM=y\nCONFIG_SPIRAM_SPEED_80M=y\n"
        self.assertEqual(self.note(sdk), "")

    def test_psram_speed_ignored_without_psram(self):
        sdk = "CONFIG_ESPTOOLPY_FLASHFREQ_80M=y\n# CONFIG_SPIRAM is not set\nCONFIG_SPIRAM_SPEED_40M=y\n"
        self.assertEqual(self.note(sdk), "")

    def test_slow_flash_alone(self):
        sdk = "CONFIG_ESPTOOLPY_FLASHFREQ_40M=y\n"
        out = self.note(sdk)
        self.assertIn("flash at 40 MHz.", out)
        self.assertNotIn("PSRAM at 40", out)

    def test_cpu_below_rating_is_named_with_the_lines_to_set(self):
        sdk = 'CONFIG_IDF_TARGET="esp32s3"\nCONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160\nCONFIG_ESPTOOLPY_FLASHFREQ_80M=y\n'
        out = self.note(sdk)
        self.assertIn("the CPU at 160 MHz (rated for 240).", out)
        self.assertIn("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y", out)
        self.assertIn("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240", out)
        self.assertNotIn("FLASHFREQ_80M", out)

    def test_cpu_with_slow_flash_and_psram(self):
        sdk = ('CONFIG_IDF_TARGET="esp32"\nCONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160\n'
               "CONFIG_ESPTOOLPY_FLASHFREQ_40M=y\nCONFIG_SPIRAM=y\nCONFIG_SPIRAM_SPEED_40M=y\n")
        self.assertIn("the CPU at 160 MHz (rated for 240), flash at 40 MHz and PSRAM at 40 MHz.", self.note(sdk))

    def test_cpu_at_rating_is_quiet(self):
        for target, mhz in (("esp32", 240), ("esp32s3", 240), ("esp32c3", 160), ("esp32c6", 160),
                            ("esp32c2", 120), ("esp32h2", 96), ("esp32p4", 400)):
            sdk = f'CONFIG_IDF_TARGET="{target}"\nCONFIG_ESP_DEFAULT_CPU_FREQ_MHZ={mhz}\nCONFIG_ESPTOOLPY_FLASHFREQ_80M=y\n'
            self.assertEqual(self.note(sdk), "", target)

    def test_p4_before_revision_3_is_rated_360(self):
        sdk = 'CONFIG_IDF_TARGET="esp32p4"\nCONFIG_ESP32P4_SELECTS_REV_LESS_V3=y\nCONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=360\n'
        self.assertEqual(self.note(sdk), "")
        sdk = 'CONFIG_IDF_TARGET="esp32p4"\nCONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=360\n'
        self.assertIn("the CPU at 360 MHz (rated for 400)", self.note(sdk))

    def test_p4_image_for_revision_0_keeps_flash_at_40(self):
        sdk = ('CONFIG_IDF_TARGET="esp32p4"\nCONFIG_ESP32P4_REV_MIN_0=y\nCONFIG_ESP32P4_SELECTS_REV_LESS_V3=y\n'
               "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=360\nCONFIG_ESPTOOLPY_FLASHFREQ_40M=y\nCONFIG_SPIRAM=y\nCONFIG_SPIRAM_SPEED_200M=y\n")
        self.assertEqual(self.note(sdk), "")

    def test_single_core_esp32_is_rated_160(self):
        sdk = 'CONFIG_IDF_TARGET="esp32"\nCONFIG_FREERTOS_UNICORE=y\nCONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160\n'
        self.assertEqual(self.note(sdk), "")


class ModulesMoved(unittest.TestCase):
    def test_nothing_moved(self):
        now = {"pydevices": row(), "displayif": row(B)}
        self.assertEqual(build_mp.modules_moved(dict(now), now), [])

    def test_commit_moved_names_module_and_both_commits(self):
        moved = build_mp.modules_moved({"pydevices": row(A)}, {"pydevices": row(B)})
        self.assertEqual(moved, [f"pydevices was at {A[:12]}, now {B[:12]}"])

    def test_uncommitted_edits_build_incrementally(self):
        # Only a commit counts: an edit to a module rebuilds incrementally.
        for had, now in ((row(), row(dirty=True, changes="123abc")),
                         (row(dirty=True, changes="123abc"), row(dirty=True, changes="456def")),
                         (row(dirty=True, changes="123abc"), row())):
            self.assertEqual(build_mp.modules_moved({"pydevices": had}, {"pydevices": now}), [])

    def test_commit_moved_with_edits_names_only_the_commits(self):
        moved = build_mp.modules_moved({"pydevices": row(A, dirty=True, changes="123abc")},
                                       {"pydevices": row(B, dirty=True, changes="123abc")})
        self.assertEqual(moved, [f"pydevices was at {A[:12]}, now {B[:12]}"])

    def test_record_without_revisions(self):
        self.assertEqual(len(build_mp.modules_moved(None, {"pydevices": row()})), 1)

    def test_module_missing_from_record(self):
        self.assertEqual(build_mp.modules_moved({}, {"pydevices": row()}), ["pydevices: not in the build record"])

    def test_not_a_git_checkout(self):
        plain = {"path": "/x", "revision": "(not a git checkout)", "commit": None, "dirty": None, "changes": None}
        self.assertEqual(build_mp.modules_moved({"mine": plain}, {"mine": dict(plain)}), [])


class BuildDirFor(unittest.TestCase):
    want = {"port": "unix", "board": None, "variant": "pydevices", "flash": None, "modules": "pydevices",
            "module_set": ["pydevices"]}

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.build = Path(self.tmp.name) / "unix" / "pydevices"

    def tearDown(self):
        self.tmp.cleanup()

    def built(self, revisions):
        """A finished build in self.build, made with `revisions`."""
        self.build.mkdir(parents=True, exist_ok=True)
        (self.build / "frozen_content.c").write_text("stale")
        rec = dict(self.want, complete=True, module_revisions=revisions)
        (self.build / "pydevices-build.json").write_text(json.dumps(rec))

    def run_it(self, revisions):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            build_mp.build_dir_for(self.build, dict(self.want), False, revisions)
        return out.getvalue()

    def test_unchanged_keeps_the_folder(self):
        self.built({"pydevices": row(A)})
        self.assertEqual(self.run_it({"pydevices": row(A)}), "")
        self.assertTrue((self.build / "frozen_content.c").exists())

    def test_moved_commit_wipes_and_says_why(self):
        self.built({"pydevices": row(A)})
        said = self.run_it({"pydevices": row(B)})
        self.assertFalse((self.build / "frozen_content.c").exists())
        self.assertIn("wiped", said)
        self.assertIn(f"pydevices was at {A[:12]}, now {B[:12]}", said)

    def test_record_written_before_the_build_carries_revisions(self):
        self.run_it({"pydevices": row(A)})
        rec = json.loads((self.build / "pydevices-build.json").read_text())
        self.assertFalse(rec["complete"])
        self.assertEqual(rec["module_revisions"], {"pydevices": row(A)})


class Record(unittest.TestCase):
    """record() tells one set of uncommitted changes from another."""

    def test_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp) / "mod"
            repo.mkdir()

            def git(*args):
                subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)

            git("init", "-q")
            (repo / "m.py").write_text("x = 1\n")
            git("add", "m.py")
            git("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "one")
            clean = build_mp.record([repo])["mod"]
            self.assertFalse(clean["dirty"])
            self.assertIsNone(clean["changes"])
            (repo / "m.py").write_text("x = 2\n")
            first = build_mp.record([repo])["mod"]
            self.assertTrue(first["dirty"])
            self.assertEqual(build_mp.record([repo])["mod"]["changes"], first["changes"])
            (repo / "m.py").write_text("x = 3\n")
            self.assertNotEqual(build_mp.record([repo])["mod"]["changes"], first["changes"])


class GitRepo:
    @staticmethod
    def git_repo(path, name="f.py"):
        path.mkdir(parents=True)

        def git(*args):
            subprocess.run(["git", "-C", str(path), *args], check=True, capture_output=True)

        git("init", "-q")
        (path / name).write_text("x = 1\n")
        git("add", name)
        git("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "one")
        return path


class Lvgl(unittest.TestCase):
    """omnipython-lvgl comes from DEPENDENCIES.lock: --modules names it lvgl, a build
    takes the port for its interpreter, and the record names the repository."""

    def checkout(self, tmp):
        root = GitRepo.git_repo(Path(tmp) / "omnipython-lvgl", "lv_conf.h")
        for port in ("micropython", "circuitpython"):
            (root / "ports" / port).mkdir(parents=True)
            (root / "ports" / port / "micropython.mk").write_text("")
        return root

    def modules_dir(self, tmp, *names):
        modules = Path(tmp) / "modules"
        for n in names:
            (modules / n).mkdir(parents=True)
        saved = build_mp.MODULES_DIR
        build_mp.MODULES_DIR = modules
        self.addCleanup(setattr, build_mp, "MODULES_DIR", saved)
        return modules

    def test_lvgl_is_the_port_for_the_interpreter(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.checkout(tmp)
            self.modules_dir(tmp, "jpegio")
            for interp in ("micropython", "circuitpython"):
                spec, dirs, _ = build_mp.resolve_modules("jpegio,lvgl", None, interp, lambda: root)
                port = (root / "ports" / interp).resolve()
                self.assertEqual(dirs[1], port)
                self.assertEqual(spec, f"jpegio,{port}")
                self.assertEqual(build_mp.lvgl_in(dirs), root.resolve())

    def test_a_path_to_the_checkout_is_the_port_too(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.checkout(tmp)
            self.modules_dir(tmp)
            _, dirs, _ = build_mp.resolve_modules(str(root), None, "circuitpython")
            self.assertEqual(dirs, [(root / "ports" / "circuitpython").resolve()])
            with self.assertRaises(SystemExit) as cm:
                build_mp.resolve_modules(str(root / "ports" / "micropython"), None, "circuitpython")
            self.assertIn("another interpreter", str(cm.exception))

    def test_all_takes_lvgl_and_fetches_nothing_otherwise(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.checkout(tmp)
            self.modules_dir(tmp, "all", "a")
            asked = []

            def lvgl():
                asked.append(1)
                return root

            build_mp.resolve_modules("a", None, "micropython", lvgl)
            self.assertEqual(asked, [])
            _, dirs, _ = build_mp.resolve_modules("all", None, "micropython", lvgl)
            self.assertIn((root / "ports" / "micropython").resolve(), dirs)

    def test_the_old_names_are_refused_naming_lvgl(self):
        for old in ("lvgl-micropython", "lvgl-circuitpython", "lvgl-bindings"):
            with self.subTest(old), self.assertRaises(SystemExit) as cm:
                build_mp.resolve_modules(f"jpegio,{old}")
            self.assertIn("--modules ...,lvgl", str(cm.exception))

    @unittest.skipIf(os.name == "nt", "the CircuitPython build is Linux or WSL")
    def test_a_circuitpython_build_refuses_an_old_name_before_fetching_anything(self):
        args = type("A", (), {"flash": False, "modules": "jpegio,/some/where/lvgl-circuitpython"})()
        with self.assertRaises(SystemExit) as cm:
            build_mp.build_circuitpython(args, [], None)
        self.assertIn("lvgl", str(cm.exception))

    def test_record_names_the_repository(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.checkout(tmp)
            mod = GitRepo.git_repo(Path(tmp) / "mod")
            rows = build_mp.record([mod, root / "ports" / "micropython"])
            self.assertEqual(sorted(rows), ["mod", "omnipython-lvgl"])
            self.assertEqual(len(rows["omnipython-lvgl"]["commit"]), 40)
            self.assertFalse(rows["omnipython-lvgl"]["dirty"])
            (root / "lv_conf.h").write_text("x = 2\n")
            self.assertTrue(build_mp.record([root / "ports" / "micropython"])["omnipython-lvgl"]["dirty"])

    def test_a_short_commit_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "modules").mkdir()
            (repo / "DEPENDENCIES.lock").write_text("omnipython-lvgl https://example.invalid/omnipython-lvgl.git 84d1de3\n")
            saved = build_mp.REPO, build_mp.MODULES_DIR
            build_mp.REPO, build_mp.MODULES_DIR = repo, repo / "modules"
            try:
                with self.assertRaises(SystemExit) as cm:
                    build_mp.lvgl_checkout(None)
            finally:
                build_mp.REPO, build_mp.MODULES_DIR = saved
            self.assertIn("full 40-character commit", str(cm.exception))


class ModulesFollowTheLock(unittest.TestCase):
    """A module build_mp.py cloned itself moves to the commit DEPENDENCIES.lock
    pins on every build, and a checkout it can't move safely stops the build."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.upstream = GitRepo.git_repo(self.tmp / "up")
        self.old = self.commit(self.upstream)
        (self.upstream / "f.py").write_text("x = 2\n")
        self.git(self.upstream, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qam", "two")
        self.new = self.commit(self.upstream)
        (self.tmp / "modules").mkdir()
        saved = build_mp.REPO, build_mp.MODULES_DIR
        build_mp.REPO, build_mp.MODULES_DIR = self.tmp, self.tmp / "modules"
        self.addCleanup(lambda: setattr(build_mp, "REPO", saved[0]) or setattr(build_mp, "MODULES_DIR", saved[1]))
        self.dest = self.tmp / "modules" / "pygraphics"

    @staticmethod
    def git(path, *args):
        subprocess.run(["git", "-C", str(path), *args], check=True, capture_output=True)

    @staticmethod
    def commit(path):
        return subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], text=True,
                              capture_output=True, check=True).stdout.strip()

    def pin_at(self, commit):
        (self.tmp / "DEPENDENCIES.lock").write_text(f"pygraphics {self.upstream.as_uri()} {commit}\n")

    def build(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            build_mp.ensure_modules(None)
        return out.getvalue()

    def test_a_cloned_module_follows_a_moved_pin(self):
        self.pin_at(self.old)
        self.build()
        self.assertEqual(self.commit(self.dest), self.old)
        self.pin_at(self.new)
        said = self.build()
        self.assertEqual(self.commit(self.dest), self.new)
        self.assertEqual((self.dest / "f.py").read_text(), "x = 2\n")
        self.assertIn(f"pygraphics: moved from {self.old[:12]} to {self.new[:12]}", said)

    def test_a_module_at_its_pin_is_left_alone(self):
        self.pin_at(self.old)
        self.build()
        self.assertNotIn("moved", self.build())

    def test_a_module_with_local_changes_stops_the_build(self):
        self.pin_at(self.old)
        self.build()
        (self.dest / "f.py").write_text("mine\n")
        self.pin_at(self.new)
        with self.assertRaises(SystemExit) as cm:
            self.build()
        self.assertIn("local changes", str(cm.exception))
        self.assertEqual((self.dest / "f.py").read_text(), "mine\n")

    def test_a_pin_that_cannot_be_fetched_stops_the_build(self):
        self.pin_at(self.old)
        self.build()
        self.pin_at("0" * 40)
        with self.assertRaises(SystemExit) as cm:
            self.build()
        self.assertIn("could not fetch", str(cm.exception))
        self.assertEqual(self.commit(self.dest), self.old)

    def test_a_linked_module_wins_over_the_pin(self):
        self.pin_at(self.new)
        (self.dest).symlink_to(self.upstream)
        self.build()
        self.assertTrue(self.dest.is_symlink())


class LibraryPackages(unittest.TestCase):
    """omnipython-lib: packages a build requires from the library tree, not modules/."""

    class FakeLibrary:
        def __init__(self, names):
            self._names = set(names)
            self.asked = 0

        def names(self):
            self.asked += 1
            return self._names

    def modules_dir(self, tmp, *names):
        modules = Path(tmp) / "modules"
        for n in names:
            (modules / n).mkdir(parents=True)
        saved = build_mp.MODULES_DIR
        build_mp.MODULES_DIR = modules
        self.addCleanup(setattr, build_mp, "MODULES_DIR", saved)
        return modules

    def test_a_module_is_a_module_and_never_asks_the_library(self):
        with tempfile.TemporaryDirectory() as tmp:
            modules = self.modules_dir(tmp, "displayif", "palettes")
            lib = self.FakeLibrary({"palettes", "appkit"})
            spec, dirs, packages = build_mp.resolve_modules("displayif,palettes", lib)
            self.assertEqual((spec, dirs, packages), ("displayif,palettes", [modules / "displayif", modules / "palettes"], []))
            self.assertEqual(lib.asked, 0)

    def test_a_name_that_is_no_module_but_a_library_package_is_required_from_the_library(self):
        with tempfile.TemporaryDirectory() as tmp:
            modules = self.modules_dir(tmp, "displayif")
            lib = self.FakeLibrary({"appkit", "esp32-s3-touch-lcd-7"})
            spec, dirs, packages = build_mp.resolve_modules("displayif,appkit,esp32-s3-touch-lcd-7", lib)
            self.assertEqual(spec, "displayif,appkit,esp32-s3-touch-lcd-7")
            self.assertEqual(dirs, [modules / "displayif"])
            self.assertEqual(packages, ["appkit", "esp32-s3-touch-lcd-7"])

    def test_a_name_in_neither_is_refused_and_lists_both(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.modules_dir(tmp, "displayif")
            with self.assertRaises(SystemExit) as cm:
                build_mp.resolve_modules("nosuch", self.FakeLibrary({"appkit"}))
            self.assertIn("no module 'nosuch'", str(cm.exception))
            self.assertIn("displayif", str(cm.exception))
            self.assertIn("appkit", str(cm.exception))

    def test_all_stays_every_module_but_the_opt_in_ones(self):
        with tempfile.TemporaryDirectory() as tmp:
            modules = self.modules_dir(tmp, "all", "a", "b", "optional")
            (modules / "optional" / "OPT_IN").write_text("why\n")
            _spec, dirs, packages = build_mp.resolve_modules("all", self.FakeLibrary({"appkit"}))
            self.assertEqual(sorted(d.name for d in dirs), ["a", "b"])
            self.assertEqual(packages, [])

    def test_a_manifest_that_names_the_library_needs_it_assembled(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            plain, wants, variant = root / "plain", root / "wants", root / "variant.py"
            plain.mkdir()
            wants.mkdir()
            (plain / "manifest.py").write_text("c_module('.')\n")
            (wants / "manifest.py").write_text('require("palettes", library="omnipython-lib")\n')
            variant.write_text("freeze('.')\n")
            self.assertFalse(build_mp.uses_library([plain], variant))
            self.assertTrue(build_mp.uses_library([plain, wants], variant))
            variant.write_text("# needs omnipython-lib\n")
            self.assertTrue(build_mp.uses_library([plain], variant))

    def test_a_short_commit_is_refused_when_the_library_must_be_cloned(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "modules").mkdir()
            (repo / "DEPENDENCIES.lock").write_text("omnipython-lib https://example.invalid/lib.git 46e1654\n")
            saved = build_mp.REPO, build_mp.MODULES_DIR
            build_mp.REPO, build_mp.MODULES_DIR = repo, repo / "modules"
            try:
                with self.assertRaises(SystemExit) as cm:
                    build_mp.library_checkout(None)
            finally:
                build_mp.REPO, build_mp.MODULES_DIR = saved
            self.assertIn("full 40-character commit", str(cm.exception))

    def test_the_workspace_checkout_of_the_library_wins_over_the_pin(self):
        with tempfile.TemporaryDirectory() as tmp:
            ws = Path(tmp) / "ws"
            (ws / "pydevices" / "tools").mkdir(parents=True)
            (ws / "pydevices" / "tools" / "libbuild.py").write_text("")
            repo = Path(tmp) / "repo"
            (repo / "modules").mkdir(parents=True)
            (repo / "DEPENDENCIES.lock").write_text("omnipython-lib https://example.invalid/lib.git " + "a" * 40 + "\n")
            saved = build_mp.REPO, build_mp.MODULES_DIR
            build_mp.REPO, build_mp.MODULES_DIR = repo, repo / "modules"
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    dest = build_mp.library_checkout(ws)
            finally:
                build_mp.REPO, build_mp.MODULES_DIR = saved
            self.assertEqual(dest.resolve(), (ws / "pydevices").resolve())

    def test_the_build_record_names_the_library_commit(self):
        with tempfile.TemporaryDirectory() as tmp:
            lib = GitRepo.git_repo(Path(tmp) / "lib", "libbuild.py")
            rows = build_mp.record([], lib)
            self.assertEqual(list(rows), ["omnipython-lib"])
            self.assertEqual(len(rows["omnipython-lib"]["commit"]), 40)


class MicropythonVersion(unittest.TestCase):
    """sys.version names the upstream commit's release, from tags a CI clone has to fetch."""

    def repo(self, tmp):
        def git(*a):
            subprocess.run(["git", "-C", tmp, "-c", "user.name=t", "-c", "user.email=t@t", *a],
                           check=True, capture_output=True)
        git("init", "-q")
        (Path(tmp) / "f").write_text("1")
        git("add", "f")
        git("commit", "-qm", "one")
        git("tag", "v1.30.0-preview")
        for n in "23":
            (Path(tmp) / "f").write_text(n)
            git("commit", "-qam", n)
        head = subprocess.run(["git", "-C", tmp, "rev-parse", "HEAD"], text=True, capture_output=True).stdout.strip()
        # The overlay, committed on top, as apply_patches.prepare does.
        (Path(tmp) / "f").write_text("overlay")
        git("commit", "-qam", "overlay")
        return head

    def test_version_of_the_upstream_commit_not_the_overlay(self):
        with tempfile.TemporaryDirectory() as tmp:
            upstream = self.repo(tmp)
            tag, short = build_mp.micropython_version(tmp, upstream)
            self.assertEqual(tag, f"v1.30.0-preview.2.g{upstream[:10]}")
            self.assertEqual(short, upstream[:10])

    def test_only_edits_beyond_the_overlay_are_dirty(self):
        with tempfile.TemporaryDirectory() as tmp:
            upstream = self.repo(tmp)
            (Path(tmp) / "f").write_text("edited")
            self.assertTrue(build_mp.micropython_version(tmp, upstream)[0].endswith(".dirty"))

    def test_no_tags_means_no_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            upstream = self.repo(tmp)
            subprocess.run(["git", "-C", tmp, "tag", "-d", "v1.30.0-preview"], check=True, capture_output=True)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertIsNone(build_mp.micropython_version(tmp, upstream))


class CircuitpythonVersion(unittest.TestCase):
    """CircuitPython's version is its git describe, which our patch series must not make -dirty."""

    def git(self, tmp, *a):
        return subprocess.run(["git", "-C", tmp, "-c", "user.name=t", "-c", "user.email=t@t", *a],
                              check=True, capture_output=True, text=True).stdout.strip()

    def repo(self, tmp, tagged=True):
        """A checkout at its tag with our patch (a.txt) laid on it uncommitted; returns the patches."""
        t = Path(tmp)
        self.git(tmp, "init", "-q")
        (t / "a.txt").write_text("one\n")
        (t / "b.txt").write_text("one\n")
        self.git(tmp, "add", ".")
        self.git(tmp, "commit", "-qm", "one")
        if tagged:
            self.git(tmp, "tag", "11.0.0-alpha.1")
        (t / "a.txt").write_text("patched\n")
        patch = t.parent / (t.name + "-0001.patch")
        patch.write_text(self.git(tmp, "diff") + "\n")
        self.addCleanup(patch.unlink)
        return [patch]

    def test_the_patch_series_does_not_make_the_version_dirty(self):
        with tempfile.TemporaryDirectory() as tmp:
            patches = self.repo(tmp)
            self.assertTrue(self.git(tmp, "describe", "--tags", "--dirty").endswith("-dirty"))  # git's own answer
            self.assertEqual(build_mp.circuitpython_version(tmp, patches), "11.0.0-alpha.1")

    def test_an_edit_beyond_the_series_is_dirty(self):
        with tempfile.TemporaryDirectory() as tmp:
            patches = self.repo(tmp)
            (Path(tmp) / "b.txt").write_text("edited\n")
            self.assertEqual(build_mp.circuitpython_version(tmp, patches), "11.0.0-alpha.1-dirty")

    def test_an_edit_to_a_file_the_series_patches_is_dirty(self):
        with tempfile.TemporaryDirectory() as tmp:
            patches = self.repo(tmp)
            (Path(tmp) / "a.txt").write_text("patched and then some\n")
            self.assertEqual(build_mp.circuitpython_version(tmp, patches), "11.0.0-alpha.1-dirty")

    def test_commits_after_the_tag_and_micropython_tags_are_named_the_way_circuitpython_does(self):
        with tempfile.TemporaryDirectory() as tmp:
            patches = self.repo(tmp)
            self.git(tmp, "stash", "-q")
            (Path(tmp) / "b.txt").write_text("two\n")
            self.git(tmp, "commit", "-qam", "two")
            self.git(tmp, "tag", "v1.29.0")  # MicroPython's tags are not CircuitPython's version
            self.git(tmp, "stash", "pop", "-q")
            short = self.git(tmp, "rev-parse", "--short", "HEAD")
            version = build_mp.circuitpython_version(tmp, patches)
            self.assertRegex(version, rf"^11\.0\.0-alpha\.1-1-g{short[:5]}")
            self.assertFalse(version.endswith("-dirty"))

    def test_no_tags_means_no_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            patches = self.repo(tmp, tagged=False)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertIsNone(build_mp.circuitpython_version(tmp, patches))


class FrozenManifest(unittest.TestCase):
    """A variant of ours, of a port or of a board, may bring its own manifest."""

    def test_variant_manifests(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            variants, modules = root / "variants", root / "modules"
            board = variants / "esp32" / "BOARD" / "WITH"
            board.mkdir(parents=True)
            (board / "mpconfigvariant.cmake").write_text("")
            (board / "manifest.py").write_text("")
            plain = variants / "esp32" / "BOARD" / "WITHOUT"
            plain.mkdir(parents=True)
            (plain / "mpconfigvariant.cmake").write_text("")
            port = variants / "unix" / "pydevices"
            port.mkdir(parents=True)
            (port / "manifest.py").write_text("")
            saved = build_mp.VARIANTS_DIR, build_mp.MODULES_DIR
            build_mp.VARIANTS_DIR, build_mp.MODULES_DIR = variants, modules
            try:
                fm = build_mp.frozen_manifest
                self.assertEqual(fm("esp32", "BOARD", "WITH"), board / "manifest.py")
                self.assertEqual(fm("esp32", "BOARD", "WITHOUT"), modules / "manifest.py")
                self.assertEqual(fm("esp32", "BOARD", None), modules / "manifest.py")
                self.assertEqual(fm("esp32", "OTHER", "WITH"), modules / "manifest.py")
                self.assertEqual(fm("unix", None, "pydevices"), port / "manifest.py")
                self.assertEqual(fm("unix", None, None), modules / "manifest.py")
            finally:
                build_mp.VARIANTS_DIR, build_mp.MODULES_DIR = saved


class BoardManifest(unittest.TestCase):
    """A board with a manifest of its own upstream gets it as the base."""

    def test_upstream_board_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            mp = Path(tmp)
            own = mp / "ports" / "stm32" / "boards" / "OWN"
            own.mkdir(parents=True)
            (own / "manifest.py").write_text('include("$(PORT_DIR)/boards/manifest.py")\n')
            (mp / "ports" / "stm32" / "boards" / "BARE").mkdir(parents=True)
            saved = build_mp.MP
            build_mp.MP = mp
            try:
                self.assertEqual(build_mp.board_manifest("stm32", "OWN"), own / "manifest.py")
                self.assertIsNone(build_mp.board_manifest("stm32", "BARE"))
                self.assertIsNone(build_mp.board_manifest("unix", None))
            finally:
                build_mp.MP = saved

    def test_the_manifest_takes_it_in_place_of_the_port(self):
        text = (build_mp.MODULES_DIR / "manifest.py").read_text()
        board = text.index("include(_board_manifest)")
        port = text.index('include("$(PORT_DIR)/boards/manifest.py")')
        self.assertLess(board, port)
        self.assertIn("elif options.base:", text[board:port])


class Rp2BoardDir(unittest.TestCase):
    """Every rp2 build gets a board directory: upstream's, linked through
    unchanged, with what every build of ours adds and a variant's files where
    the port looks for them."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.base = root / "mp" / "BOARD"
        self.base.mkdir(parents=True)
        for name, text in (("mpconfigboard.cmake", "set(A 1)"), ("mpconfigboard.h", "#define NAME 1"),
                           ("mpconfigvariant.cmake", "set(P 1)"), ("pins.csv", "GP0,GPIO0"), ("board.json", "{}")):
            (self.base / name).write_text(text)
        self.ours = root / "variants" / "rp2" / "BOARD" / "OURS"
        self.ours.mkdir(parents=True)
        (self.ours / "mpconfigvariant.cmake").write_text("set(Q 2)")
        self.gen = root / "gen"
        self.saved = build_mp.VARIANTS_DIR
        build_mp.VARIANTS_DIR = root / "variants"

    def tearDown(self):
        build_mp.VARIANTS_DIR = self.saved
        self.tmp.cleanup()

    def board_cmake(self):
        return (build_mp.VARIANTS_DIR / "rp2" / "board.cmake").resolve().as_posix()

    def test_every_build_gets_our_board_settings_after_upstreams(self):
        build_mp.rp2_board_dir(self.gen, self.base, "BOARD", None)
        text = (self.gen / "mpconfigboard.cmake").read_text()
        self.assertFalse((self.gen / "mpconfigboard.cmake").is_symlink())
        self.assertIn(f"include({self.base.as_posix()}/mpconfigboard.cmake)", text)
        self.assertLess(text.index(self.base.as_posix()), text.index(self.board_cmake()))
        for name in ("mpconfigboard.h", "mpconfigvariant.cmake", "pins.csv", "board.json"):
            self.assertEqual(self.gen.joinpath(name).resolve(), self.base.joinpath(name).resolve(), name)
        self.assertEqual(sorted(p.name for p in self.gen.glob("mpconfigvariant_*")), [])

    def test_an_upstream_variant_is_linked_through(self):
        (self.base / "mpconfigvariant_UP.cmake").write_text("set(U 1)")
        build_mp.rp2_board_dir(self.gen, self.base, "BOARD", "UP")
        self.assertTrue((self.gen / "mpconfigvariant_UP.cmake").is_symlink())
        self.assertIn(self.board_cmake(), (self.gen / "mpconfigboard.cmake").read_text())

    def test_variant_file_only(self):
        build_mp.rp2_board_dir(self.gen, self.base, "BOARD", "OURS")
        self.assertIn(self.board_cmake(), (self.gen / "mpconfigboard.cmake").read_text())
        for name in ("mpconfigboard.h", "mpconfigvariant.cmake", "pins.csv", "board.json"):
            self.assertEqual(self.gen.joinpath(name).resolve(), self.base.joinpath(name).resolve(), name)
            self.assertTrue(self.gen.joinpath(name).is_symlink(), name)
        text = (self.gen / "mpconfigvariant_OURS.cmake").read_text()
        self.assertIn(f"include({self.base.as_posix()}/mpconfigvariant.cmake OPTIONAL)", text)
        self.assertLess(text.index("mpconfigvariant.cmake OPTIONAL"), text.index(self.ours.as_posix()))
        self.assertIn(f"include({self.ours.as_posix()}/mpconfigvariant.cmake)", text)

    def test_our_pins_and_header_replace_and_follow_upstreams(self):
        (self.ours / "pins.csv").write_text("GP47,GPIO47")
        (self.ours / "mpconfigboard.h").write_text("#undef NAME")
        build_mp.rp2_board_dir(self.gen, self.base, "BOARD", "OURS")
        self.assertEqual((self.gen / "pins.csv").resolve(), (self.ours / "pins.csv").resolve())
        header = (self.gen / "mpconfigboard.h").read_text()
        self.assertFalse((self.gen / "mpconfigboard.h").is_symlink())
        self.assertLess(header.index(self.base.as_posix()), header.index(self.ours.as_posix()))
        self.assertEqual((self.base / "pins.csv").read_text(), "GP0,GPIO0")  # upstream untouched

    def test_regenerates_over_an_old_directory(self):
        build_mp.rp2_board_dir(self.gen, self.base, "BOARD", "OURS")
        (self.ours / "pins.csv").write_text("GP47,GPIO47")
        build_mp.rp2_board_dir(self.gen, self.base, "BOARD", "OURS")
        self.assertEqual((self.gen / "pins.csv").resolve(), (self.ours / "pins.csv").resolve())

    def test_a_variant_that_is_not_ours_is_refused(self):
        with self.assertRaises(SystemExit):
            build_mp.rp2_board_dir(self.gen, self.base, "BOARD", "NOPE")


class Rp2KeepFs(unittest.TestCase):
    """A Pico's filesystem moves when autosize works its size out again for a
    bigger image; --keep-fs holds it where a board already has it."""

    @staticmethod
    def overflow(image):
        # The linker's memory table for a 2 MB Pico at upstream's 1408 KB filesystem.
        return ("region `FLASH' overflowed by %d bytes\n"
                "Memory region         Used Size  Region Size  %%age Used\n"
                "           FLASH:     %d B       640 KB    100.00%%\n"
                "        FLASH_FS:          0 B      1408 KB      0.00%%\n" % (image - 640 * 1024, image))

    def test_a_bigger_image_moves_the_filesystem(self):
        # Two RPI_PICO images with the same modules, 62 KB apart, autosized from scratch.
        self.assertEqual(build_mp.rp2_autosize(self.overflow(1398144)), 576 * 1024)
        self.assertEqual(build_mp.rp2_autosize(self.overflow(1460560)), 512 * 1024)
        # The bigger one fits beside the smaller one's 576 KB filesystem all the same.
        self.assertLess(1460560, 2048 * 1024 - 576 * 1024)

    def test_sizes(self):
        self.assertEqual(build_mp.fs_size("589824"), 589824)
        self.assertEqual(build_mp.fs_size("576K"), 589824)
        self.assertEqual(build_mp.fs_size("576kb"), 589824)
        self.assertEqual(build_mp.fs_size("1M"), 1 << 20)

    def test_not_sizes(self):
        for text in ("", "0", "576.5K", "1000", "-4K", "big"):
            self.assertIsNone(build_mp.fs_size(text), text)


class Rp2Speed(unittest.TestCase):
    """Every rp2 build runs its CPU at its chip's rated clock, and one that
    runs below it says so after it."""

    RP2040 = "PICO_PLATFORM:STRING=rp2040\n"
    RP2350 = "PICO_PLATFORM:STRING=rp2350-arm-s\n"

    def note(self, cache, flags):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            build_mp.rp2_speed_note(cache, flags)
        return out.getvalue()

    def test_our_board_settings_ask_for_the_fastest_clock(self):
        text = (build_mp.REPO / "variants" / "rp2" / "board.cmake").read_text()
        self.assertRegex(text, r"(?m)^set\(PICO_USE_FASTEST_SUPPORTED_CLOCK 1\)$")

    def test_rp2040_at_the_sdk_default_is_named(self):
        self.assertEqual(build_mp.rp2_cpu(self.RP2040, "C_DEFINES = -DPICO_RP2040=1"), (125, 200))
        self.assertIn("the CPU at 125 MHz; its chip is rated for 200.", self.note(self.RP2040, "C_DEFINES = -DPICO_RP2040=1"))

    def test_rp2040_at_the_fastest_clock_is_quiet(self):
        flags = "C_DEFINES = -DPICO_BOARD=\\\"pico\\\" -DPICO_USE_FASTEST_SUPPORTED_CLOCK=1 -DPICO_XIP_RAM=0"
        self.assertEqual(build_mp.rp2_cpu(self.RP2040, flags), (200, 200))
        self.assertEqual(self.note(self.RP2040, flags), "")

    def test_a_clock_given_outright_wins(self):
        flags = "-DPICO_USE_FASTEST_SUPPORTED_CLOCK=1 -DSYS_CLK_MHZ=133"
        self.assertIn("the CPU at 133 MHz", self.note(self.RP2040, flags))
        self.assertEqual(build_mp.rp2_cpu(self.RP2040, "-DSYS_CLK_HZ=200000000"), (200, 200))
        self.assertEqual(build_mp.rp2_cpu(self.RP2040, "-DSYS_CLK_KHZ=200000"), (200, 200))

    def test_rp2350_at_its_default_is_quiet(self):
        self.assertEqual(build_mp.rp2_cpu(self.RP2350, ""), (150, 150))
        self.assertEqual(self.note(self.RP2350, ""), "")

    def test_a_build_that_does_not_say_is_quiet(self):
        self.assertEqual(build_mp.rp2_cpu("", ""), (None, None))
        self.assertEqual(self.note("", ""), "")


class Esp32Autosize(unittest.TestCase):
    """An esp32 app that outgrows its partition gets a table grown to fit, with
    headroom above it, and always leaves the board a filesystem."""

    TABLE = ("# Name, Type, SubType, Offset, Size, Flags\n"
             "nvs, data, nvs, 0x9000, 0x6000\n"
             "phy_init, data, phy, 0xf000, 0x1000\n"
             "factory, app, factory, 0x10000, 0x1F0000\n")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.build = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def grow(self, image, mb=4, table=None):
        src = self.build / "partitions.csv"
        src.write_text(table or self.TABLE)
        (self.build / "sdkconfig").write_text(
            f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="{src}"\nCONFIG_ESPTOOLPY_FLASHSIZE="{mb}MB"\n')
        log = (f"Error: app partition is too small for binary micropython.bin size {hex(image)}:\n"
               "  - Part 'factory' 0/0 @ 0x10000 size 0x1f0000 (overflow 0x10)\n")
        dest = self.build / "grown.csv"
        build_mp.esp32_autosize(log, self.build, self.build, dest)
        rows = [r.split(", ") for r in dest.read_text().splitlines() if r and not r.startswith("#")]
        return {r[0]: (int(r[3], 0), int(r[4], 0)) for r in rows}

    def test_room_to_spare_keeps_the_headroom(self):
        parts = self.grow(0x2F0000)
        self.assertEqual(parts["factory"], (0x10000, 0x310000))

    def test_the_headroom_shrinks_to_leave_the_filesystem(self):
        # 0x3c1bc0 fits 0x3d0000; the 128 KB headroom would end the app at the
        # end of a 4 MB flash and leave no filesystem at all.
        offset, size = self.grow(0x3C1BC0)["factory"]
        self.assertEqual(size, 0x3D0000)
        self.assertEqual(0x400000 - (offset + size), build_mp.ESP32_MIN_VFS)

    def test_an_image_that_leaves_too_little_filesystem_fails_the_build(self):
        with self.assertRaises(SystemExit) as caught:
            self.grow(0x3D8000)
        self.assertIn("filesystem", str(caught.exception))

    def test_a_named_filesystem_partition_keeps_the_minimum(self):
        table = self.TABLE + "vfs, data, fat, 0x200000, 0x600000\n"
        parts = self.grow(0x7C0000, mb=8, table=table)
        offset, size = parts["vfs"]
        self.assertEqual(offset + size, 0x800000)
        self.assertGreaterEqual(size, build_mp.ESP32_MIN_VFS)
        self.assertEqual(parts["factory"][1], 0x7D0000)


class Esp32KeepFs(unittest.TestCase):
    """--keep-fs on esp32 sizes the app partition so the filesystem is the last
    SIZE of the flash, where a board flashed with the same size has it."""

    TABLE = Esp32Autosize.TABLE

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def keep(self, fs, mb=8, table=None, src=None):
        if src is None:
            src = self.dir / "partitions.csv"
            src.write_text(table or self.TABLE)
        dest = build_mp.esp32_keep_fs(src, mb * 1024 * 1024, fs, self.dir / "kept.csv")
        return {r[0]: (int(r[3], 0), int(r[4], 0)) for r in build_mp.esp32_table_rows(dest)}

    def test_the_release_cells_keep_the_tables_their_last_set_had(self):
        # The app partitions the release workflow's set of 2026-11-09 shipped with,
        # each grown by autosize but the DEV-KIT's, and the Touch-LCD-4B's grown
        # 256 KB more for esp_cam_sensor 2.7.0's JX-H63P.
        shipped = {"mp-p4-devkit": 0x600000, "mp-p4-panel": 0x5F0000, "mp-s3-lcd7": 0x540000,
                   "mp-s3-lcd43": 0x540000, "mp-s3-tembed": 0x540000}
        for c in rf.CELLS:
            if c["interpreter"] != "micropython" or c["kind"] != "esp32":
                continue
            table = build_mp.esp32_table(c["chip"], c["board"], c["variant"])
            self.assertIsNotNone(table, f"{c['id']} needs a table build_mp.py knows before it builds")
            parts = self.keep(rf.size_bytes(c["fs"]), int(c["flash"][:-2]), src=table)
            self.assertEqual(parts["factory"], (0x10000, shipped[c["id"]]), c["id"])

    def test_a_table_kept_at_its_own_size_is_unchanged(self):
        for variant, fs, mb in (("ESP32_GENERIC_P4/P4X_EYE", 0x6F0000, 16), ("ESP32_GENERIC/AUDIO_KIT", 0x20000, 4)):
            src = build_mp.VARIANTS_DIR / "esp32" / variant / "partitions.csv"
            before = {r[0]: (int(r[3], 0), int(r[4], 0)) for r in build_mp.esp32_table_rows(src)}
            self.assertEqual(self.keep(fs, mb, src=src), before, variant)

    def test_partitions_after_the_app_follow_it(self):
        parts = self.keep(2688 * 1024, table=self.TABLE + "coredump, data, coredump, 0x200000, 0x10000\n")
        self.assertEqual(parts["factory"], (0x10000, 0x540000))
        self.assertEqual(parts["coredump"], (0x550000, 0x10000))

    def test_a_named_filesystem_is_the_end_of_the_flash(self):
        parts = self.keep(0x200000, table=self.TABLE + "vfs, data, fat, 0x200000, 0x200000\n")
        self.assertEqual(parts["vfs"], (0x600000, 0x200000))
        self.assertEqual(parts["factory"], (0x10000, 0x5F0000))

    def test_an_image_that_outgrows_it_is_told_what_fits(self):
        table = self.dir / "kept.csv"
        self.keep(2688 * 1024, table=self.TABLE + "coredump, data, coredump, 0x200000, 0x10000\n")
        log = "Error: app partition is too small for binary micropython.bin size 0x560000:\n"
        # 0x560000 of app, the coredump after it: 0x800000 - 0x10000 - 0x560000 - 0x10000 is left.
        self.assertEqual(build_mp.esp32_keep_fs_fits(log, table, 0x800000), 0x280000)
        self.assertEqual(build_mp.esp32_keep_fs_fits(log.replace("0x560000", "0x7F0000"), table, 0x800000), 0)
        self.assertIsNone(build_mp.esp32_keep_fs_fits("error: something else", table, 0x800000))

    def test_tables_it_cannot_size_are_refused(self):
        ota = self.TABLE.replace("factory, app, factory", "ota_0, app, ota_0") + "ota_1, app, ota_1, 0x200000, 0x1F0000\n"
        middle = self.TABLE + "vfs, data, fat, 0x200000, 0x100000\ncoredump, data, coredump, 0x300000, 0x10000\n"
        for table in (ota, middle):
            with self.assertRaises(SystemExit):
                self.keep(0x100000, table=table)
        with self.assertRaises(SystemExit):
            self.keep(0x7F0000)   # no room left for the app


class Stm32M4Slot(unittest.TestCase):
    """An Arduino GIGA firmware that overflows its 1,280 KB region is built
    again over the M4 core's flash, without OpenAMP; and no GIGA image may
    leave a runnable vector table where the M4 starts at reset."""

    LD = ("MEMORY\n{\n"
          "    FLASH_TEXT (rx) : ORIGIN = 0x08040000, LENGTH = 1280K   /* CM7 firmware */\n"
          "    FLASH_CM4 (rx)  : ORIGIN = 0x08180000, LENGTH = 512K    /* CM4 firmware */\n"
          "}\n")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.base = root / "mp" / "ARDUINO_GIGA"
        self.base.mkdir(parents=True)
        (self.base / "stm32h747.ld").write_text(self.LD)
        (self.base / "mpconfigboard.mk").write_text("MICROPY_PY_OPENAMP = 1")
        self.build = root / "build"
        self.gen = self.build / "ARDUINO_GIGA"

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_overflow_is_read_from_the_linkers_message(self):
        log = "arm-none-eabi/bin/ld: region `FLASH_TEXT' overflowed by 172404 bytes\ncollect2: error"
        self.assertEqual(build_mp.stm32_overflow(log), 172404)
        self.assertIsNone(build_mp.stm32_overflow("region `RAM' overflowed by 12 bytes"))

    def test_the_grown_region_covers_the_m4_slot_and_drops_openamp(self):
        build_mp.stm32_plain_board_dir(self.gen, self.base)
        build_mp.stm32_m4_slot(self.build, self.gen, self.base, "ARDUINO_GIGA", None)
        ld = (self.build / "stm32h747_m4_slot.ld").read_text()
        self.assertIn("ORIGIN = 0x08040000, LENGTH = 1792K", ld)
        self.assertNotIn("FLASH_CM4", ld)
        mk = (self.build / build_mp.M4_SLOT_MK).read_text()
        self.assertIn("MICROPY_PY_OPENAMP = 0", mk)
        self.assertIn("MICROPY_PY_OPENAMP_REMOTEPROC = 0", mk)
        self.assertIn("$(patsubst boards/ARDUINO_GIGA/stm32h747.ld,", mk)
        self.assertIn(f"include {(self.build / build_mp.M4_SLOT_MK).as_posix()}",
                      (self.gen / "mpconfigvariant.mk").read_text())
        self.assertEqual((self.base / "stm32h747.ld").read_text(), self.LD)  # upstream untouched

    def test_a_linker_script_laid_out_otherwise_is_refused(self):
        (self.base / "stm32h747.ld").write_text("MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 2048K }")
        build_mp.stm32_plain_board_dir(self.gen, self.base)
        with self.assertRaises(SystemExit):
            build_mp.stm32_m4_slot(self.build, self.gen, self.base, "ARDUINO_GIGA", None)

    # The vector table (stack, reset, NMI, HardFault) at 0x08100000 in each
    # GIGA image we had on 11-06, and whether the build must refuse it.
    IMAGES = {
        "lean, ran": ((0xF880B508, 0xF8D021FC, 0x68522214, 0x20004790), False),
        "display+audio release, ran": ((0x1A3FFF9B, 0x4404D0F3, 0x407FF425, 0x0080F020), False),
        "display+audio, earlier": ((0xF0232100, 0xB118BC3F, 0x718CF44F, 0xBF48F009), False),
        "display+audio+USB, ran": ((0xF5044629, 0xF7FC70E5, 0x4633FB7A, 0xE7349A05), False),
        "autosize test, HardFault in SDRAM": ((0x4E2A300D, 0xF043BF18, 0xF8DF0301, 0x603A80B4), True),
        "ulab, hung at the DAC": ((0xD0522E00, 0x92014630, 0xF98AF03D, 0x9A011A20), True),
        "ulab moved 16 bytes, ran": ((0xF8030302, 0xBB4D1C01, 0x1E9C2130, 0x1C02F803), False),
    }

    def _image(self, words):
        image = self.build / "firmware0.bin"
        self.build.mkdir(parents=True, exist_ok=True)
        off = build_mp.STM32_M4_BOOT_DEFAULT - 0x08040000
        image.write_bytes(bytes(off) + struct.pack("<IIII", *words))
        return image

    def test_the_m4_boot_words(self):
        image = self._image((1, 2, 3, 4))
        self.assertEqual(build_mp.stm32_m4_boot_words(image, 0x08040000), (1, 2, 3, 4))
        image.write_bytes(bytes(build_mp.STM32_M4_BOOT_DEFAULT - 0x08040000))
        self.assertIsNone(build_mp.stm32_m4_boot_words(image, 0x08040000))

    def test_each_image_we_had_is_refused_or_passed(self):
        for name, (words, refused) in self.IMAGES.items():
            with self.subTest(name):
                self.assertEqual(build_mp.m4_reaches_memory(*words), refused)

    def test_a_harmless_reset_with_a_hardfault_handler_in_flash_is_refused(self):
        self.assertTrue(build_mp.m4_reaches_memory(0xF0000000, 0xF0000001, 0, 0x08100101))

    def test_a_stack_in_memory_is_refused_whatever_the_reset_address(self):
        self.assertTrue(build_mp.m4_reaches_memory(0x24080000, 0xF0000001, 0, 0xF0000001))
        self.assertTrue(build_mp.m4_reaches_memory(0xD0522E00, 0xF0000001, 0, 0xF0000001))

    def test_the_m7s_tightly_coupled_memory_is_out_of_the_m4s_reach(self):
        self.assertFalse(build_mp.m4_reaches_memory(0x20010000, 0x00000101, 0, 0x20004791))

    # Where the padding function sits in the fake images below, well before the
    # M4's address, as it does in a real GIGA image.
    PAD_AT = 0x080405ED

    def _code(self, windows):
        """An image whose code before the M4's address is *windows*, 16-byte
        vector tables in order, the last one at the address."""
        off = build_mp.STM32_M4_BOOT_DEFAULT - 0x08040000
        body = b"".join(struct.pack("<IIII", *w) for w in windows)
        image = self.build / "firmware0.bin"
        self.build.mkdir(parents=True, exist_ok=True)
        image.write_bytes(bytes(off + 16 - len(body)) + body + bytes(64))
        return image

    def test_a_refused_image_is_padded_until_one_passes(self):
        ulab = self.IMAGES["ulab, hung at the DAC"][0]
        moved = self.IMAGES["ulab moved 16 bytes, ran"][0]
        # 64 bytes of literal pool full of flash addresses, and code before it.
        pool = (0x081F3C58, 0x081DF094, 0x081DD7B0, 0x081DCF18)
        windows = [moved] + [pool] * 4 + [ulab]
        image = self._code(windows)
        links = []

        def relink(pad):
            links.append(pad)
            # Each 16 bytes of padding brings the window before to the address.
            self._code(windows[:len(windows) - pad // 16])
            return 0

        pad, words, refused = build_mp.stm32_m4_settle(image, 0x08040000, 0, lambda: self.PAD_AT, relink)
        self.assertEqual((pad, words, refused), (80, moved, ulab))
        self.assertEqual(links, [80])  # read off the image: one relink, not five

    def test_a_prediction_the_relink_doesnt_bear_out_is_tried_again(self):
        ulab = self.IMAGES["ulab, hung at the DAC"][0]
        moved = self.IMAGES["ulab moved 16 bytes, ran"][0]
        image = self._code([moved, moved, ulab])
        links = []

        def relink(pad):
            links.append(pad)
            # The first relink moves the code less than the image said it would.
            self._code([moved, ulab] if pad == 16 else [ulab, moved])
            return 0

        pad, words, _refused = build_mp.stm32_m4_settle(image, 0x08040000, 0, lambda: self.PAD_AT, relink)
        self.assertEqual(links, [16, 32])
        self.assertEqual((pad, words), (32, moved))

    def test_an_image_that_passes_is_not_relinked(self):
        image = self._image(self.IMAGES["lean, ran"][0])
        pad, _words, refused = build_mp.stm32_m4_settle(image, 0x08040000, 0, lambda: self.PAD_AT,
                                                        lambda p: self.fail("relinked"))
        self.assertEqual((pad, refused), (0, None))

    def test_a_smaller_padding_is_taken_when_it_passes(self):
        ulab = self.IMAGES["ulab, hung at the DAC"][0]
        lean = self.IMAGES["lean, ran"][0]
        # Linked with 48 bytes; with 32 the window after the address comes back.
        off = build_mp.STM32_M4_BOOT_DEFAULT - 0x08040000
        image = self.build / "firmware0.bin"
        self.build.mkdir(parents=True, exist_ok=True)
        after = [ulab, lean, ulab, ulab, ulab]
        image.write_bytes(bytes(off) + b"".join(struct.pack("<IIII", *w) for w in after) + bytes(64))
        self.assertEqual(build_mp.stm32_m4_pad_candidates(image, 0x08040000, 48, self.PAD_AT)[0], 32)

    def test_no_padding_that_passes_leaves_the_image_refused(self):
        ulab = self.IMAGES["ulab, hung at the DAC"][0]
        image = self._code([ulab] * (build_mp.M4_PAD_MAX // 16 + 1))
        pad, words, _refused = build_mp.stm32_m4_settle(image, 0x08040000, 0, lambda: self.PAD_AT,
                                                        lambda p: self.fail("relinked"))
        self.assertEqual(pad, 0)
        self.assertTrue(build_mp.m4_reaches_memory(*words))

    def test_padding_after_the_m4s_address_cant_move_it(self):
        image = self._code([self.IMAGES["lean, ran"][0], self.IMAGES["ulab, hung at the DAC"][0]])
        self.assertIsNone(build_mp.stm32_m4_pad_candidates(image, 0x08040000, 0, 0x08100101))
        self.assertIsNone(build_mp.stm32_m4_pad_candidates(image, 0x08040000, 0, None))

    def test_the_padding_module(self):
        d = build_mp.stm32_m4_pad_module(self.build, 0)
        mk = (d / "micropython.mk").read_text()
        # Compiled even when empty, so the next build can find where it is.
        self.assertIn("SRC_USERMOD_C += $(USERMOD_DIR)/m4pad.c", mk)
        self.assertIn("-Wl,-u," + build_mp.M4_PAD_SYMBOL, mk)
        self.assertIn(".rept 0", (d / "m4pad.c").read_text())
        self.assertEqual(build_mp.stm32_m4_pad_now(self.build), 0)
        build_mp.stm32_m4_pad_module(self.build, 48)
        self.assertIn(".rept 24", (d / "m4pad.c").read_text())
        self.assertEqual(build_mp.stm32_m4_pad_now(self.build), 48)

    def test_a_symbol_is_read_from_an_elf_file(self):
        names = b"\0" + build_mp.M4_PAD_SYMBOL.encode() + b"\0other\0"
        syms = struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0) + struct.pack("<IIIBBH", 1, self.PAD_AT, 8, 0x12, 0, 1)
        shstr = b"\0"
        head = bytearray(52)
        head[:6] = b"\x7fELF\x01\x01"
        strtab_off = 52
        symtab_off = strtab_off + len(names)
        shoff = symtab_off + len(syms)
        struct.pack_into("<I", head, 0x20, shoff)
        struct.pack_into("<HH", head, 0x2E, 40, 3)
        sh = (bytes(40)
              + struct.pack("<IIIIIIIIII", 0, 3, 0, 0, strtab_off, len(names), 0, 0, 1, 0)
              + struct.pack("<IIIIIIIIII", 0, 2, 0, 0, symtab_off, len(syms), 1, 1, 4, 16))
        elf = self.build / "firmware.elf"
        self.build.mkdir(parents=True, exist_ok=True)
        elf.write_bytes(bytes(head) + names + syms + sh + shstr)
        self.assertEqual(build_mp.elf_symbol(elf, build_mp.M4_PAD_SYMBOL), self.PAD_AT)
        self.assertIsNone(build_mp.elf_symbol(elf, "missing"))
        self.assertIsNone(build_mp.elf_symbol(self.build / "nothing.elf", "x"))


class PythonToRomfs(unittest.TestCase):
    """A GIGA build compiles the Python its manifest left unfrozen, and packs it for the board's ROMFS."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        # A stand-in mpy-cross that records its arguments in the file it writes.
        self.cross = self.root / "mpy-cross"
        self.cross.write_text("#!/bin/sh\nfor a; do case $prev in -o) out=$a;; esac; prev=$a; done\necho \"$@\" > $out\n")
        self.cross.chmod(0o755)
        (self.root / "src").mkdir()
        (self.root / "src" / "core.py").write_text("x = 1\n")
        (self.root / "src" / "data.mpy").write_bytes(b"M\x06")

    def tearDown(self):
        self.dir.cleanup()

    def test_compiles_sources_and_copies_mpy_files_where_they_import(self):
        listing = self.root / "romfs-python.json"
        listing.write_text(json.dumps([[str(self.root / "src" / "core.py"), "asyncio/core.py", None],
                                       [str(self.root / "src" / "core.py"), "fast.py", 3],
                                       [str(self.root / "src" / "data.mpy"), "data.mpy", None]]))
        out = self.root / "romfs"
        with contextlib.redirect_stdout(io.StringIO()):
            listed = build_mp.python_to_romfs(listing, out, self.cross, "armv7emdp")
        self.assertEqual(sorted(p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file()),
                         ["asyncio/core.mpy", "data.mpy", "fast.mpy"])
        self.assertEqual(listed, ["asyncio/core.mpy", "data.mpy", "fast.mpy"])
        self.assertIn("-march=armv7emdp -s asyncio/core.py", (out / "asyncio" / "core.mpy").read_text())
        self.assertIn("-O3", (out / "fast.mpy").read_text())
        self.assertEqual((out / "data.mpy").read_bytes(), b"M\x06")

    def test_a_build_whose_manifest_wrote_no_list_is_refused(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            build_mp.python_to_romfs(self.root / "missing.json", self.root / "romfs", self.cross, "armv7emdp")

    def test_the_modules_are_packed_by_the_checkouts_own_romfs_writer(self):
        writer = self.root / "mp" / "tools" / "mpremote" / "mpremote"
        writer.mkdir(parents=True)
        (writer / "romfs.py").write_text(
            "import os\n"
            "def make_romfs(src_dir, *, mpy_cross):\n"
            "    print('a tree nobody needs to see')\n"
            "    return repr((sorted(os.listdir(src_dir)), mpy_cross)).encode()\n")
        out = self.root / "romfs.img"
        with contextlib.redirect_stdout(io.StringIO()) as said:
            build_mp.romfs_image(self.root / "mp", self.root / "src", out)
        self.assertEqual(out.read_bytes(), repr((["core.py", "data.mpy"], False)).encode())
        self.assertNotIn("a tree", said.getvalue())

    def test_the_giga_compiles_for_its_h7(self):
        mp = self.root / "mp"
        board = mp / "ports" / "stm32" / "boards" / "ARDUINO_GIGA"
        board.mkdir(parents=True)
        (board / "mpconfigboard.mk").write_text("USE_MBOOT = 0\nMCU_SERIES = h7\n")
        self.assertEqual(build_mp.mpy_cross_arch(mp, "stm32", "ARDUINO_GIGA"), "armv7emdp")


class Stm32BoardDir(unittest.TestCase):
    """A variant of ours on stm32 gets a board directory: upstream's, linked
    through unchanged, a variant file that includes ours, our headers after
    the board's, and our other files beside the board's."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.base = root / "mp" / "BOARD"
        self.base.mkdir(parents=True)
        for name, text in (("mpconfigboard.mk", "A = 1"), ("mpconfigboard.h", "#define NAME 1"),
                           ("stm32h7xx_hal_conf.h", "// hal"), ("board.ld", "/* ld */"), ("manifest.py", "")):
            (self.base / name).write_text(text)
        self.ours = root / "variants" / "stm32" / "BOARD" / "OURS"
        self.ours.mkdir(parents=True)
        (self.ours / "mpconfigvariant.mk").write_text("B = 2")
        self.gen = root / "gen"
        self.saved = build_mp.VARIANTS_DIR
        build_mp.VARIANTS_DIR = root / "variants"

    def tearDown(self):
        build_mp.VARIANTS_DIR = self.saved
        self.tmp.cleanup()

    def test_variant_file_only(self):
        build_mp.stm32_board_dir(self.gen, self.base, "BOARD", "OURS")
        for name in ("mpconfigboard.mk", "mpconfigboard.h", "stm32h7xx_hal_conf.h", "board.ld"):
            self.assertEqual(self.gen.joinpath(name).resolve(), self.base.joinpath(name).resolve(), name)
            self.assertTrue(self.gen.joinpath(name).is_symlink(), name)
        text = (self.gen / "mpconfigvariant_OURS.mk").read_text()
        self.assertIn(f"include {self.ours.as_posix()}/mpconfigvariant.mk", text)

    def test_our_headers_follow_the_boards_and_other_files_sit_beside_them(self):
        (self.ours / "stm32h7xx_hal_conf.h").write_text("#define MORE 1")
        (self.ours / "extra.ld").write_text("/* ours */")
        (self.ours / "manifest.py").write_text("")
        build_mp.stm32_board_dir(self.gen, self.base, "BOARD", "OURS")
        header = (self.gen / "stm32h7xx_hal_conf.h").read_text()
        self.assertFalse((self.gen / "stm32h7xx_hal_conf.h").is_symlink())
        self.assertLess(header.index(self.base.as_posix()), header.index(self.ours.as_posix()))
        self.assertEqual((self.gen / "extra.ld").resolve(), (self.ours / "extra.ld").resolve())
        # The variant's manifest is the build's FROZEN_MANIFEST, not a board file.
        self.assertEqual((self.gen / "manifest.py").resolve(), (self.base / "manifest.py").resolve())
        self.assertEqual((self.base / "stm32h7xx_hal_conf.h").read_text(), "// hal")  # upstream untouched

    def test_a_file_of_ours_may_not_replace_one_of_the_boards(self):
        (self.ours / "board.ld").write_text("/* ours */")
        with self.assertRaises(SystemExit):
            build_mp.stm32_board_dir(self.gen, self.base, "BOARD", "OURS")

    def test_a_variant_that_is_not_ours_is_refused(self):
        with self.assertRaises(SystemExit):
            build_mp.stm32_board_dir(self.gen, self.base, "BOARD", "NOPE")


class CircuitPythonFrozenLibraries(unittest.TestCase):
    """A CircuitPython build that freezes our Python freezes asyncio and
    adafruit_ticks too, unless the board already does."""

    def test_both_when_the_board_freezes_neither(self):
        dirs = build_mp.cp_frozen_library_dirs(Path("/cp"), [])
        self.assertEqual([Path(d).name for d in dirs],
                         ["Adafruit_CircuitPython_asyncio", "Adafruit_CircuitPython_Ticks"])

    def test_not_again_what_the_board_has(self):
        dirs = build_mp.cp_frozen_library_dirs(Path("/cp"), ["/cp/frozen/Adafruit_CircuitPython_asyncio"])
        self.assertEqual([Path(d).name for d in dirs], ["Adafruit_CircuitPython_Ticks"])

    def test_nothing_when_the_board_has_both(self):
        both = [f"/cp/frozen/{n}" for n in build_mp.CP_FROZEN_LIBRARIES]
        self.assertEqual(build_mp.cp_frozen_library_dirs(Path("/cp"), both), [])


class ForeignSources(unittest.TestCase):
    """foreign_sources() names an object compiled from outside its module, and only that."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.build = self.root / "build"
        self.module = self.root / "repo" / "modules" / "displayif"
        (self.module / "src").mkdir(parents=True)

    def tearDown(self):
        self.tmp.cleanup()

    def dep(self, obj, src):
        p = self.build / obj
        p.parent.mkdir(parents=True, exist_ok=True)
        # What mkrules.mk leaves: gcc's rule, then a "header :" line per header.
        p.write_text(f"{self.build}/{obj[:-2]}.o: \\\n {src} \\\n /usr/include/stdio.h\n/usr/include/stdio.h :\n")

    def test_own_module_core_and_absolute_objects_pass(self):
        self.dep("displayif/src/a.P", self.module / "src" / "a.c")
        self.dep("py/gc.P", "../../py/gc.c")
        self.dep("main.P", "main.c")
        # A source outside every module keeps its whole path under BUILD.
        self.dep(f"{str(self.root).lstrip('/')}/bindings/lv.P", self.root / "bindings" / "lv.c")
        self.assertEqual(build_mp.foreign_sources(self.build, [self.module]), [])

    def test_a_stray_copy_beside_another_module_is_named(self):
        stray = self.root / "displayif" / "src" / "a.c"
        self.dep("displayif/src/a.P", stray)
        self.dep("displayif/src/b.P", self.module / "src" / "b.c")
        found = build_mp.foreign_sources(self.build, [self.module, self.root / "audiodsp"])
        self.assertEqual(found, [(self.build / "displayif/src/a.P", str(stray))])

    def test_a_source_the_build_generated_in_its_own_directory_passes(self):
        # stm32's OpenAMP copies libmetal's sources into BUILD/openamp/metal/.
        self.dep("openamp/metal/device.P", self.build / "openamp" / "metal" / "device.c")
        self.assertEqual(build_mp.foreign_sources(self.build, [self.module]), [])

    def test_a_module_reached_through_a_link_is_its_own(self):
        link = self.root / "repo" / "modules" / "linked"
        link.symlink_to(self.module, target_is_directory=True)
        self.dep("displayif/src/a.P", self.module / "src" / "a.c")
        self.assertEqual(build_mp.foreign_sources(self.build, [link]), [])


class VpathSearchesEachModuleFirst(unittest.TestCase):
    """make finds a user C module's sources in its own directory, not in a
    directory of the same name beside another module's parent (an older
    checkout of it). Upstream's search list holds every module's parent,
    sorted, so a parent that sorts first wins; patches/micropython/0040 puts
    each module's own directory first.

    Needs micropython/ prepared with the overlay (any build does it); skipped
    without one, unless PYDEVICES_REQUIRE_MICROPYTHON is set, as CI does."""

    def mp(self):
        mp = Path(build_mp.__file__).resolve().parent / "micropython"
        why = None
        if os.name == "nt":
            why = "the unix port's Makefile on native Windows"
        elif not (mp / "py" / "py.mk").is_file():
            why = "no micropython/ checkout"
        elif "The PyDevices overlay applied" not in build_mp.git_out(mp, "log", "-1", "--format=%s"):
            why = "micropython/ is not prepared with the overlay (run a build, or patches/apply_patches.py)"
        if why:
            if os.environ.get("PYDEVICES_REQUIRE_MICROPYTHON"):
                self.fail(why)
            self.skipTest(why)
        return mp

    def resolve(self, mp, layout):
        """Where make finds mod/x.c, in the unix port, for a build of `sib` and `repo/modules/mod`."""
        probe = layout / "probe.mk"
        probe.write_text('include Makefile\nvpath-probe: mod/x.c\n\t@echo "PROBE $<"\n')
        mods = f"{layout / 'sib'} {layout / 'repo' / 'modules' / 'mod'}"
        out = subprocess.run(["make", "-s", "-C", str(mp / "ports" / "unix"), "-f", str(probe),
                              f"BUILD={layout / 'build'}", "FROZEN_MANIFEST=", f"USER_C_MODULES={mods}",
                              "vpath-probe"], capture_output=True, text=True, timeout=60)
        got = [l[6:] for l in out.stdout.splitlines() if l.startswith("PROBE ")]
        self.assertEqual(len(got), 1, out.stdout + out.stderr)
        return Path(got[0])

    def test_the_module_own_copy_wins(self):
        mp = self.mp()
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            (t / "sib").mkdir()
            (t / "sib" / "micropython.mk").write_text("# puts its parent on the search list\n")
            ours = t / "repo" / "modules" / "mod"
            ours.mkdir(parents=True)
            (ours / "micropython.mk").write_text("SRC_USERMOD_C += $(USERMOD_DIR)/x.c\n")
            (ours / "x.c").write_text("int ours;\n")
            self.assertEqual(self.resolve(mp, t), ours / "x.c")  # control: nothing stray
            (t / "mod").mkdir()
            (t / "mod" / "x.c").write_text("#error stray copy\n")  # beside sib, sorted first
            self.assertEqual(self.resolve(mp, t), ours / "x.c")


class MicropythonLibPatches(unittest.TestCase):
    """patches/micropython-lib/ goes onto the submodule once, and a second pass leaves it be."""

    def test_applies_once(self):
        import apply_patches

        patches = apply_patches.lib_series()
        self.assertTrue(patches)
        files = apply_patches.lib_files(patches)
        self.assertIn("micropython/usb/usb-device-midi/usb/device/midi.py", files)
        pin = "ee4bb8ff139e24c42b739935fbd8ec7c4d061e02"  # the submodule at b70ae1aca
        mp = Path(build_mp.__file__).resolve().parent / "micropython"
        with tempfile.TemporaryDirectory() as tmp:
            lib = Path(tmp)

            def git(*args, repo=lib):
                return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True).stdout

            git("init", "-q")
            for f in files:
                (lib / f).parent.mkdir(parents=True, exist_ok=True)
                try:
                    (lib / f).write_bytes(git("show", f"{pin}:{f}", repo=mp / "lib" / "micropython-lib"))
                except subprocess.CalledProcessError:
                    self.skipTest("no micropython-lib checkout at the pin")
            git("add", "-A")
            git("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "pin")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                apply_patches.apply_lib(lib, patches)
            self.assertEqual(out.getvalue().count("applied "), len(patches))
            midi = (lib / "micropython/usb/usb-device-midi/usb/device/midi.py").read_text()
            self.assertIn("_on_rx_schedule", midi)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                apply_patches.apply_lib(lib, patches)
            self.assertEqual(out.getvalue().count("already in micropython-lib"), len(patches))
            self.assertEqual((lib / "micropython/usb/usb-device-midi/usb/device/midi.py").read_text(), midi)


class Coredump(unittest.TestCase):
    """A standard esp32 build has no flash core dump; --coredump adds one, with
    the partition it writes to. Reads the generated sdkconfig and the table
    build_mp.py would pick, so nothing is built."""

    ON = "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y"
    cells = [c for c in rf.CELLS if c["interpreter"] == "micropython" and c["port"] == "esp32"]

    def fragment(self, c, coredump):
        _, module_dirs, _ = build_mp.resolve_modules(c["modules"])
        text, _ = build_mp.esp32_fragment(c["chip"], module_dirs, c["board"], c["variant"], c["flash"], None, coredump)
        return text

    def test_the_release_cells_cover_both_chips(self):
        self.assertEqual({c["chip"] for c in self.cells}, {"esp32s3", "esp32p4"})

    def test_standard_builds_have_no_coredump(self):
        for c in self.cells:
            with self.subTest(c["id"]):
                self.assertNotIn("COREDUMP", self.fragment(c, False))

    def test_the_opt_in_turns_it_on_last(self):
        for c in self.cells:
            with self.subTest(c["id"]):
                text = self.fragment(c, True)
                self.assertIn(self.ON, text)
                # kconfgen takes the last assignment: nothing may follow our lines
                self.assertEqual(text.rstrip().splitlines()[-1], "CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y")

    def test_only_sdkconfig_coredump_names_it(self):
        here = Path(build_mp.__file__).resolve().parent
        for f in list((here / "variants").rglob("sdkconfig*")) + list((here / "modules").rglob("sdkconfig*")):
            if f.name != "sdkconfig.coredump":
                with self.subTest(str(f.relative_to(here))):
                    self.assertNotIn("COREDUMP", f.read_text())

    def test_tables_are_the_same_with_or_without_the_flag_and_only_the_s3_has_the_slot(self):
        # build_mp.py picks one table whatever the flag says; a standard S3 image
        # and a troubleshooting one share it, so the filesystem doesn't move. A
        # P4 table, where there is one, has no coredump slot, so --coredump
        # refuses there.
        for c in self.cells:
            table = build_mp.esp32_table(c["chip"], c["board"], c["variant"])
            with self.subTest(c["id"]):
                if c["chip"] == "esp32s3":
                    self.assertTrue(build_mp.has_coredump_partition(table))
                else:
                    self.assertFalse(table and build_mp.has_coredump_partition(table))

    def test_a_table_without_the_partition_is_seen(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp) / "p.csv"
            t.write_text("nvs, data, nvs, 0x9000, 0x6000,\nfactory, app, factory, 0x10000, 0x1F0000,\n"
                         "# coredump, data, coredump, 0x200000, 0x10000,\n")
            self.assertFalse(build_mp.has_coredump_partition(t))
            t.write_text(t.read_text() + "coredump, data, coredump, 0x200000, 0x10000,\n")
            self.assertTrue(build_mp.has_coredump_partition(t))


class CircuitPythonMakeArgs(unittest.TestCase):
    """Only CircuitPython's unix port asks for four-byte qstr offsets."""

    def test_unix_asks_and_boards_do_not(self):
        self.assertEqual(build_mp.cp_make_args("unix", ["CIRCUITPY_X=0"]),
                         ["CIRCUITPY_X=0", "CFLAGS_EXTRA=-DMICROPY_QSTR_OFFSET_BYTES=4"])
        for port in ("espressif", "raspberrypi", "nordic"):
            self.assertEqual(build_mp.cp_make_args(port, ["CIRCUITPY_X=0"]), ["CIRCUITPY_X=0"])

    def test_a_callers_cflags_extra_keeps_its_flags(self):
        self.assertEqual(build_mp.cp_make_args("unix", ["CFLAGS_EXTRA=-DFOO"]),
                         ["CFLAGS_EXTRA=-DFOO -DMICROPY_QSTR_OFFSET_BYTES=4"])


if __name__ == "__main__":
    unittest.main()
