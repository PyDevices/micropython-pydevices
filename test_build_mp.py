"""Unit tests for build_mp.py's decisions that need no build.

    python3 -m unittest test_build_mp -v
"""

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import build_mp

sys.path.insert(0, str(Path(__file__).resolve().parent / "scripts"))
import release_firmware as rf

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
            (repo / "modules.lock").write_text("omnipython-lib https://example.invalid/lib.git 46e1654\n")
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
            (repo / "modules.lock").write_text("omnipython-lib https://example.invalid/lib.git " + "a" * 40 + "\n")
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
            lib = LvglBindings.git_repo(None, Path(tmp) / "lib", "libbuild.py")
            rows = build_mp.record([], None, lib)
            self.assertEqual(list(rows), ["omnipython-lib"])
            self.assertEqual(len(rows["omnipython-lib"]["commit"]), 40)


class RefuseLvglMicropython(unittest.TestCase):
    """CircuitPython's LVGL binding is lvgl-circuitpython, not lvgl-micropython."""

    def test_lvgl_micropython_is_refused_naming_lvgl_circuitpython(self):
        with self.assertRaises(SystemExit) as cm:
            build_mp.refuse_lvgl_micropython(["jpegio", "lvgl-micropython"])
        self.assertIn("lvgl-circuitpython", str(cm.exception))
        self.assertIn("--modules", str(cm.exception))

    def test_the_circuitpython_binding_and_other_modules_pass(self):
        build_mp.refuse_lvgl_micropython(["jpegio", "lvgl-circuitpython", ""])

    @unittest.skipIf(os.name == "nt", "the CircuitPython build is Linux or WSL")
    def test_a_circuitpython_build_refuses_before_fetching_anything(self):
        args = type("A", (), {"flash": False, "modules": "jpegio,/some/where/lvgl-micropython"})()
        with self.assertRaises(SystemExit) as cm:
            build_mp.build_circuitpython(args, [], None)
        self.assertIn("lvgl-circuitpython", str(cm.exception))


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
        # and a troubleshooting one share it, so the filesystem doesn't move. The
        # P4 has no table of ours, so --coredump refuses there.
        for c in self.cells:
            table = build_mp.esp32_table(c["chip"], c["board"], c["variant"])
            with self.subTest(c["id"]):
                if c["chip"] == "esp32s3":
                    self.assertTrue(build_mp.has_coredump_partition(table))
                else:
                    self.assertIsNone(table)

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
