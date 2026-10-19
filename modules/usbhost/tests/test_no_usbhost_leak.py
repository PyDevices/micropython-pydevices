# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""`_usbhost` is the packages' to call, and this is what keeps it that way.

The C module is the transfer and stream layer under `usb.core` and the
`usb.host.*` packages. An application that reaches past them into `_usbhost`
is writing code that runs on exactly one firmware, and once that was how
every example in this repository was written (PyDevices/usbif#10).

So the rule is drawn here rather than left to reviewers: nothing an application reads -- the examples,
the root README's snippets, the examples README -- may name it at all. Which package files may import it is
checked beside the packages, in pydevices. The
test walks the files instead of trusting a convention, because the twenty
examples that once broke this rule each looked perfectly reasonable alone.
Tools and hardware checks under `tools/` and `tests/` may name it: they
measure the layer itself.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

_IMPORT = re.compile(r"^\s*(?:import\s+_usbhost|from\s+_usbhost\s+import)", re.M)
_ANY_USE = re.compile(r"\b_usbhost\s*\.")


def _fenced_python(text):
    """The ```python blocks of a markdown file, joined."""
    return "\n".join(re.findall(r"```python\n(.*?)```", text, re.S))


class NoLeakTests(unittest.TestCase):
    def test_no_example_names_the_c_module(self):
        offenders = []
        for path in sorted((ROOT / "examples").rglob("*.py")):
            text = path.read_text()
            if _IMPORT.search(text) or _ANY_USE.search(text):
                offenders.append(path.relative_to(ROOT).as_posix())
        self.assertEqual(
            offenders, [],
            "these examples reach into the C module; go through usb.core and the "
            "usb.host packages: " + ", ".join(offenders))

    def test_no_application_facing_doc_shows_the_c_module(self):
        offenders = []
        for rel in ("README.md", "examples/README.md"):
            path = ROOT / rel
            if not path.is_file():
                continue
            code = _fenced_python(path.read_text())
            if _IMPORT.search(code) or _ANY_USE.search(code):
                offenders.append(rel)
        self.assertEqual(
            offenders, [],
            "these docs teach applications to import the C module: "
            + ", ".join(offenders))


if __name__ == "__main__":
    unittest.main()
