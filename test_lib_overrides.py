"""lib-overrides/mip is micropython-lib's mip plus the marked patch, nothing else."""
import pathlib
import re
import unittest

HERE = pathlib.Path(__file__).parent
OURS = HERE / "lib-overrides/mip/mip/__init__.py"
_REL = pathlib.Path("micropython/lib/micropython-lib/micropython/mip/mip/__init__.py")
UPSTREAM = next((d / _REL for d in HERE.resolve().parents if (d / _REL).exists()), HERE / "missing")


def strip_patch(text):
    text = re.sub(r"\n# --- PyDevices patch begins.*?# --- PyDevices patch ends ---\n\n", "\n", text, flags=re.S)
    out = []
    for line in text.split("\n"):
        if line.endswith("_drop()  # PyDevices"):
            continue
        if "# PyDevices: was requests.get" in line:
            line = line.replace("_get(", "requests.get(").replace("  # PyDevices: was requests.get", "")
        out.append(line)
    return re.sub(r"\n{3,}", "\n\n", "\n".join(out))


class MipOverride(unittest.TestCase):
    @unittest.skipUnless(UPSTREAM.exists(), "no micropython-lib checkout beside this repo")
    def test_differs_from_upstream_only_in_the_patch(self):
        upstream = re.sub(r"\n{3,}", "\n\n", UPSTREAM.read_text())
        self.assertEqual(strip_patch(OURS.read_text()), upstream)


if __name__ == "__main__":
    unittest.main()
