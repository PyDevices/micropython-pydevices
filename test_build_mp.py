"""Unit tests for build_mp.py's decisions that need no build.

    python3 -m unittest test_build_mp -v
"""

import contextlib
import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

import build_mp

A = "a" * 40
B = "b" * 40


def row(commit=A, dirty=False, changes=None):
    return {"path": "/x", "revision": commit[:12], "commit": commit, "dirty": dirty, "changes": changes}


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


class LvglBindings(unittest.TestCase):
    """lvgl-bindings comes from modules.lock and lands in the build record."""

    def git_repo(self, path, name="f.py"):
        path.mkdir(parents=True)

        def git(*args):
            subprocess.run(["git", "-C", str(path), *args], check=True, capture_output=True)

        git("init", "-q")
        (path / name).write_text("x = 1\n")
        git("add", name)
        git("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "one")
        return path

    def test_not_fetched_without_lvgl_micropython(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(build_mp.lvgl_bindings(None, [Path(tmp) / "ulab"]))

    def test_record_names_the_bindings(self):
        with tempfile.TemporaryDirectory() as tmp:
            mod = self.git_repo(Path(tmp) / "mod")
            bindings = self.git_repo(Path(tmp) / ".lvgl-bindings", "lv_conf.h")
            rows = build_mp.record([mod], bindings)
            self.assertEqual(sorted(rows), ["lvgl-bindings", "mod"])
            self.assertEqual(len(rows["lvgl-bindings"]["commit"]), 40)
            self.assertFalse(rows["lvgl-bindings"]["dirty"])
            (bindings / "lv_conf.h").write_text("x = 2\n")
            self.assertTrue(build_mp.record([mod], bindings)["lvgl-bindings"]["dirty"])

    def test_a_short_commit_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "modules").mkdir()
            (repo / "modules.lock").write_text("lvgl-bindings https://example.invalid/lvgl-bindings.git 7740c2f\n")
            saved = build_mp.REPO, build_mp.MODULES_DIR
            build_mp.REPO, build_mp.MODULES_DIR = repo, repo / "modules"
            try:
                with self.assertRaises(SystemExit) as cm:
                    build_mp.lvgl_bindings(None, [repo / "modules" / "lvgl-micropython"])
            finally:
                build_mp.REPO, build_mp.MODULES_DIR = saved
            self.assertIn("full 40-character commit", str(cm.exception))


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


if __name__ == "__main__":
    unittest.main()
