"""Unit tests for scripts/freeze_list.py's comparison and, with a prepared
MicroPython checkout, its reading of a manifest.

    python3 -m unittest test_freeze_list -v
"""

import contextlib
import importlib.util
import io
import json
import os
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("freeze_list", HERE / "scripts" / "freeze_list.py")
freeze_list = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(freeze_list)


def entry(source="s", frozen="f", opt=3, package=None, version=None):
    return {"source_sha256": source, "frozen_sha256": frozen, "opt": opt, "package": package, "version": version}


def write(path, files, packages=None):
    path.write_text(json.dumps({"files": files, "packages": packages or {}, "c_modules": []}))
    return str(path)


class Compare(unittest.TestCase):
    def compare(self, before, after, *flags):
        with tempfile.TemporaryDirectory() as tmp:
            a = write(Path(tmp) / "a.json", before)
            b = write(Path(tmp) / "b.json", after)
            args = type("A", (), {"before": a, "after": b, "version_lines": bool(flags)})()
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = freeze_list.cmd_compare(args)
            return code, out.getvalue()

    def test_the_same_files_are_no_difference(self):
        code, out = self.compare({"a.py": entry()}, {"a.py": entry()})
        self.assertEqual(code, 0)
        self.assertIn("identical frozen content:                        1", out)

    def test_an_appended_version_line_is_told_apart_and_does_not_fail(self):
        code, out = self.compare({"a.py": entry(frozen="x")}, {"a.py": entry(frozen="y")})
        self.assertEqual(code, 0)
        self.assertIn("different appended __version__: 1", out)

    def test_different_source_fails_and_is_named(self):
        code, out = self.compare({"a.py": entry(source="1", frozen="1")}, {"a.py": entry(source="2", frozen="2")})
        self.assertEqual(code, 1)
        self.assertIn("DIFFERENT: a.py", out)

    def test_a_file_in_only_one_list_fails(self):
        code, out = self.compare({"a.py": entry()}, {"b.py": entry()})
        self.assertEqual(code, 1)
        self.assertIn("ONLY BEFORE: a.py", out)
        self.assertIn("ONLY AFTER: b.py", out)

    def test_a_different_optimisation_level_fails(self):
        code, out = self.compare({"a.py": entry(opt=3)}, {"a.py": entry(opt=None)})
        self.assertEqual(code, 1)
        self.assertIn("OPT: a.py", out)


MICROPYTHON = HERE / "micropython"


@unittest.skipUnless((MICROPYTHON / "tools" / "manifestfile.py").is_file(), "needs a MicroPython checkout")
class Evaluate(unittest.TestCase):
    def test_it_lists_a_required_packages_files_with_the_version_it_tags_them_with(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "lib" / "pkg").mkdir(parents=True)
            (root / "lib" / "pkg" / "manifest.py").write_text('metadata(version="1.2.3")\npackage("pkg", opt=options.opt)\n')
            (root / "lib" / "pkg" / "pkg").mkdir()
            (root / "lib" / "pkg" / "pkg" / "__init__.py").write_text("x = 1\n")
            (root / "top.py").write_text("x = 2\n")
            manifest = root / "manifest.py"
            manifest.write_text(
                'add_library("L", os.environ["TEST_LIB"])\n'
                'require("pkg", library="L", opt=3)\n'
                'module("top.py", base_path=os.path.dirname(os.environ["TEST_LIB"]))\n'
            )
            manifest.write_text("import os\n" + manifest.read_text())
            doc = freeze_list.evaluate(str(MICROPYTHON), str(root), None, None, {"TEST_LIB": str(root / "lib")}, str(manifest))
            self.assertEqual(doc["packages"], {"pkg": "1.2.3"})
            tagged = doc["files"]["pkg/__init__.py"]
            self.assertEqual((tagged["opt"], tagged["version"]), (3, "1.2.3"))
            self.assertNotEqual(tagged["source_sha256"], tagged["frozen_sha256"])
            self.assertEqual(doc["files"]["top.py"]["source_sha256"], doc["files"]["top.py"]["frozen_sha256"])


if __name__ == "__main__":
    unittest.main()
