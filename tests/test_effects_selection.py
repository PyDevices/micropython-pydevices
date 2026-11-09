# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Unit tests for modules/audioeffects/selection.py: which files a few named
effects need, and the package it writes for them.

    PYTHONPATH=tests python3 -m unittest test_effects_selection -v
"""

import ast
import os
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "modules" / "audioeffects"))
import selection  # noqa: E402

# A small package shaped like audioeffects: a lazy __init__, the shared base,
# two plain effects, a rack that builds on one of them, and one nobody picks.
INIT = '''"""Effects."""

from . import _component

_MODULES = {
    "Chorus": "chorus",
    "Reverb": "reverb",
    "ShimmerHall": "shimmerhall",
    "Compressor": "compressor",
}


def __getattr__(name):
    module = _MODULES.get(name)
    if module is None:
        raise AttributeError(name)
    cls = getattr(__import__("audioeffects." + module, None, None, [name]), name)
    globals()[name] = cls
    return cls


__all__ = [
    "create", "ALL",
    "Chorus", "Reverb", "ShimmerHall", "Compressor",
]


def create(name, source, rate, **options):
    if name not in _MODULES:
        raise ImportError("audioeffects has no %s" % name)
    return __getattr__(name).create(source, rate, **options)


ALL = tuple(sorted(_MODULES))
'''

EAGER_INIT = '''from .chorus import Chorus
from .reverb import Reverb
ALL = ("Chorus", "Reverb")
'''

FILES = {
    "_component.py": "import math\n\nclass Component:\n    NAME = None\n",
    "chorus.py": "from . import _component\n\nclass Chorus(_component.Component):\n    NAME = 'Chorus'\n",
    "reverb.py": "import math\nfrom . import _component\n\nclass Reverb(_component.Component):\n    NAME = 'Reverb'\n",
    "shimmerhall.py": "from . import _component\nfrom . import reverb as _reverb\n\nclass ShimmerHall(_component.Component):\n    NAME = 'ShimmerHall'\n",
    "compressor.py": "from . import _component\n\nclass Compressor(_component.Component):\n    NAME = 'Compressor'\n",
}


class Selection(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.dir.name, "src", "audioeffects")
        os.makedirs(self.root)
        self.write("__init__.py", INIT)
        for name, text in FILES.items():
            self.write(name, text)
        self.out = os.path.join(self.dir.name, "out")

    def tearDown(self):
        self.dir.cleanup()

    def write(self, name, text):
        with open(os.path.join(self.root, name), "w", encoding="utf-8") as f:
            f.write(text)

    def written_init(self):
        with open(os.path.join(self.out, "audioeffects", "__init__.py"), encoding="utf-8") as f:
            return ast.parse(f.read())

    def assigned(self, tree, name):
        node = next(n for n in tree.body if isinstance(n, ast.Assign) and n.targets[0].id == name)
        return ast.literal_eval(node.value)

    def test_named_effects_take_the_base_and_nothing_else(self):
        self.assertEqual(selection.files(self.root, ["chorus", "reverb"]),
                         ["__init__.py", "_component.py", "chorus.py", "reverb.py"])

    def test_a_class_name_in_any_case_picks_its_module(self):
        self.assertEqual(selection.resolve(self.root, ["Reverb", "CHORUS", "reverb"]), ["reverb", "chorus"])

    def test_a_rack_brings_the_effect_it_builds_on(self):
        self.assertIn("reverb.py", selection.files(self.root, ["shimmerhall"]))

    def test_an_unknown_name_stops_and_lists_the_effects(self):
        with self.assertRaises(ValueError) as caught:
            selection.files(self.root, ["reverb", "warp"])
        message = str(caught.exception)
        self.assertIn("warp", message)
        for module in ("chorus", "compressor", "reverb", "shimmerhall"):
            self.assertIn(module, message)

    def test_an_eager_package_is_refused(self):
        # An audioeffects that imports every effect when it loads can't be
        # frozen in part: the build must stop rather than freeze a package
        # whose import fails on the board.
        self.write("__init__.py", EAGER_INIT)
        with self.assertRaises(ValueError) as caught:
            selection.write(self.root, ["reverb"], self.out)
        self.assertIn("first use", str(caught.exception))

    def test_the_written_package_lists_only_the_selection(self):
        written = selection.write(self.root, ["chorus", "reverb"], self.out)
        self.assertEqual(sorted(os.path.basename(p) for p in written),
                         ["__init__.py", "_component.py", "chorus.py", "reverb.py"])
        tree = self.written_init()
        self.assertEqual(self.assigned(tree, "_MODULES"), {"Chorus": "chorus", "Reverb": "reverb"})
        self.assertEqual(self.assigned(tree, "__all__"), ["create", "ALL", "Chorus", "Reverb"])

    def test_an_effect_a_rack_needs_is_in_the_written_table(self):
        selection.write(self.root, ["shimmerhall"], self.out)
        self.assertEqual(self.assigned(self.written_init(), "_MODULES"),
                         {"Reverb": "reverb", "ShimmerHall": "shimmerhall"})

    def test_the_written_package_imports_and_refuses_the_rest(self):
        selection.write(self.root, ["reverb"], self.out)
        sys.path.insert(0, self.out)
        try:
            import audioeffects
            self.assertEqual(audioeffects.ALL, ("Reverb",))
            self.assertEqual(audioeffects.Reverb.NAME, "Reverb")
            with self.assertRaises(AttributeError):
                _ = audioeffects.Compressor
            with self.assertRaises(ImportError):
                audioeffects.create("Compressor", None, 48000)
        finally:
            sys.path.remove(self.out)
            for name in [m for m in sys.modules if m == "audioeffects" or m.startswith("audioeffects.")]:
                del sys.modules[name]

    def test_writing_again_replaces_the_last_selection(self):
        selection.write(self.root, ["chorus", "reverb"], self.out)
        selection.write(self.root, ["reverb"], self.out)
        self.assertFalse(os.path.exists(os.path.join(self.out, "audioeffects", "chorus.py")))


class Manifest(unittest.TestCase):
    """The manifest reads AUDIOEFFECTS the way the instruments' reads AUDIOINSTRUMENTS."""

    def test_unset_or_empty_freezes_the_whole_package(self):
        text = (REPO / "modules" / "audioeffects" / "manifest.py").read_text()
        self.assertIn('os.environ.get("AUDIOEFFECTS", "")', text)
        self.assertIn('require("audioeffects", library="omnipython-lib", opt=3)', text)
        self.assertIn("if n.strip()", text)


if __name__ == "__main__":
    unittest.main()
