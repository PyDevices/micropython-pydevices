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


if __name__ == "__main__":
    unittest.main()
