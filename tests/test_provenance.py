"""Tests for scripts/update_provenance.py, and the check that the committed
provenance.json is current.

    PYTHONPATH=tests python3 -m unittest test_provenance -v
"""
import contextlib
import io
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import update_provenance as up

REPO = Path(__file__).resolve().parent.parent


class Committed(unittest.TestCase):
    def test_provenance_json_matches_the_tree(self):
        # Fails when a patch or UPSTREAM changed and the file wasn't regenerated.
        self.assertEqual(up.main(["--check"]), 0, "run: python3 scripts/update_provenance.py")


class Generator(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        for rel in ("patches/micropython", "patches/tinyusb"):
            (self.dir / rel).mkdir(parents=True)
        (self.dir / "patches/micropython/0001-a.patch").write_text("a\n")
        (self.dir / "patches/tinyusb/0001-t.patch").write_text("t\n")
        (self.dir / "UPSTREAM").write_text("b70ae1aca657bb93e2857dac4e2a2d21a3803200\n")
        (self.dir / "provenance.json").write_text(
            json.dumps({"upstream": "micropython/micropython main at b70ae1aca657 (v1.30.0-preview-160)",
                        "migrated-from": "history"}))
        self.run_main()

    def run_main(self, *argv):
        with contextlib.redirect_stdout(io.StringIO()):
            return up.main(list(argv), self.dir)

    def data(self):
        return json.loads((self.dir / "provenance.json").read_text())

    def test_generated_file_is_current_and_keeps_history(self):
        self.assertEqual(self.run_main("--check"), 0)
        d = self.data()
        self.assertEqual(d["migrated-from"], "history")
        self.assertEqual(list(d["patches"]), ["0001-a.patch"])
        self.assertEqual(len(d["patches"]["0001-a.patch"]["sha256"]), 64)

    def test_edited_patch_is_stale(self):
        (self.dir / "patches/micropython/0001-a.patch").write_text("changed\n")
        self.assertEqual(self.run_main("--check"), 1)
        self.run_main()
        self.assertEqual(self.run_main("--check"), 0)

    def test_added_and_removed_patches_are_stale(self):
        (self.dir / "patches/tinyusb/0002-t.patch").write_text("x\n")
        self.assertEqual(self.run_main("--check"), 1)
        self.run_main()
        (self.dir / "patches/tinyusb/0002-t.patch").unlink()
        self.assertEqual(self.run_main("--check"), 1)

    def test_moved_pin_needs_a_name_for_it(self):
        (self.dir / "UPSTREAM").write_text("c" * 40 + "\n")
        self.assertEqual(self.run_main("--check"), 1)
        with self.assertRaises(SystemExit):
            self.run_main()
        self.run_main("--describe", "v1.30.0-preview-171")
        self.assertEqual(self.data()["upstream"], "micropython/micropython main at cccccccccccc (v1.30.0-preview-171)")
        self.assertEqual(self.run_main("--check"), 0)


if __name__ == "__main__":
    unittest.main()
