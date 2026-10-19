# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""`_usbif` is the packages' to call, and this is what keeps it that way.

The C module is the transfer and stream layer under `usb.core` and the
`usb.host.*` packages. An application that reaches past them into `_usbif`
is writing code that runs on exactly one firmware, and once that was how
every example in this repository was written (PyDevices/usbif#10).

So the rule is drawn here rather than left to reviewers: four package files
may import the C module, and nothing an application reads -- the examples,
the root README's snippets, the examples README -- may name it at all. The
test walks the files instead of trusting a convention, because the twenty
examples that once broke this rule each looked perfectly reasonable alone.
Tools and hardware checks under `tools/` and `tests/` may name it: they
measure the layer itself.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

# The package files that front the C module, and the only ones allowed to
# import it. Every other package builds on usb.core.
ALLOWED = {
    ROOT / "packages" / "usb-core" / "usb" / "core.py",
    ROOT / "packages" / "usb-host-audio" / "usb" / "host" / "audio.py",
    ROOT / "packages" / "usb-host-video" / "usb" / "host" / "video.py",
    ROOT / "packages" / "usb-host-net" / "usb" / "host" / "net.py",
}

_IMPORT = re.compile(r"^\s*(?:import\s+_usbif|from\s+_usbif\s+import)", re.M)
_ANY_USE = re.compile(r"\b_usbif\s*\.")


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

    def test_only_the_fronting_package_files_import_the_c_module(self):
        found = set()
        for path in sorted((ROOT / "packages").rglob("*.py")):
            if _IMPORT.search(path.read_text()):
                found.add(path)
        unexpected = sorted(p.relative_to(ROOT).as_posix() for p in found - ALLOWED)
        self.assertEqual(
            unexpected, [],
            "only usb.core, usb.host.audio, usb.host.video and usb.host.net may "
            "import _usbif: " + ", ".join(unexpected))
        # And the list doesn't outlive what it names.
        self.assertEqual(sorted(p.relative_to(ROOT).as_posix() for p in ALLOWED - found), [])


if __name__ == "__main__":
    unittest.main()
