"""Tests for scripts/stage_firmware_page.py.

    python3 -m unittest test_stage_firmware_page -v
"""
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_firmware as rf
import stage_firmware_page as sp
from test_release_firmware import fake_set

INTERP = {"tag": "v0.5.5", "files": {"micropython": {"sha256": "0" * 64, "size": 1, "platform": "linux-x86_64",
                                                     "version": "x"}}}


class Stage(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        indir = self.root / "in"
        indir.mkdir()
        fake_set(indir)
        self.fw = self.root / "fw"
        self.doc = rf.assemble(indir, self.fw, False, "staged-20261020", "https://run", "b" * 40)
        self.page = self.root / "page"

    def test_files_manifests_and_firmware_json_land(self):
        sp.stage(self.fw, self.page)
        served = {f["name"] for b in self.doc["builds"] for f in b["files"] if f["role"] in ("flash", "uf2")}
        self.assertEqual({p.name for p in (self.page / "files").iterdir()}, served)
        self.assertEqual(len(served), 9)
        self.assertEqual(json.loads((self.page / "firmware.json").read_text()), self.doc)
        ids = sorted(p.stem for p in (self.page / "manifests").iterdir())
        self.assertEqual(ids, sorted(b["id"] for b in self.doc["builds"] if b["chip"] != "rp2040"))
        self.assertFalse((self.page / "interpreters.json").exists())

    def test_the_manifest_writes_the_image_at_its_flash_offset(self):
        sp.stage(self.fw, self.page)
        m = json.loads((self.page / "manifests" / "mp-p4-devkit.json").read_text())
        self.assertEqual(m["builds"][0]["chipFamily"], "ESP32-P4")
        self.assertEqual(m["builds"][0]["parts"][0]["offset"], 0x2000)
        self.assertTrue(m["new_install_prompt_erase"])
        path = m["builds"][0]["parts"][0]["path"]
        self.assertTrue((self.page / "manifests" / path).resolve().is_file())
        m = json.loads((self.page / "manifests" / "cp-p4-devkit.json").read_text())
        self.assertEqual(m["builds"][0]["parts"][0]["offset"], 0)
        m = json.loads((self.page / "manifests" / "mp-s3-lcd7.json").read_text())
        self.assertEqual(m["builds"][0]["chipFamily"], "ESP32-S3")

    def test_a_second_set_replaces_the_first(self):
        sp.stage(self.fw, self.page)
        (self.page / "files" / "old-set.bin").write_bytes(b"x")
        (self.page / "manifests" / "old.json").write_text("{}")
        sp.stage(self.fw, self.page, INTERP)
        self.assertFalse((self.page / "files" / "old-set.bin").exists())
        self.assertFalse((self.page / "manifests" / "old.json").exists())
        self.assertEqual(json.loads((self.page / "interpreters.json").read_text())["tag"], "v0.5.5")
        sp.stage(self.fw, self.page)
        self.assertFalse((self.page / "interpreters.json").exists())

    def test_a_changed_file_is_refused(self):
        name = next(f["name"] for b in self.doc["builds"] for f in b["files"] if f["role"] == "flash")
        with open(self.fw / name, "r+b") as f:
            f.write(b"\x00")
        with self.assertRaises(sp.Failed) as cm:
            sp.stage(self.fw, self.page)
        self.assertIn("sha256", str(cm.exception))
        self.assertFalse(self.page.exists())

    def test_a_missing_file_is_refused(self):
        name = next(f["name"] for b in self.doc["builds"] for f in b["files"] if f["role"] == "uf2")
        (self.fw / name).unlink()
        with self.assertRaises(sp.Failed):
            sp.stage(self.fw, self.page)

    def test_interpreters_without_a_tag_are_refused(self):
        with self.assertRaises(sp.Failed):
            sp.stage(self.fw, self.page, {"files": {}})

    def test_newest_final_with_interpreters(self):
        asset = [{"name": "interpreters.json"}]
        rel = [{"tag_name": "v0.7.7.dev1", "prerelease": True, "assets": asset},
               {"tag_name": "v0.7.6", "assets": [{"name": "x.whl"}]},
               {"tag_name": "v0.7.5", "draft": True, "assets": asset},
               {"tag_name": "v0.5.5", "assets": asset},
               {"tag_name": "v0.5.4", "assets": asset}]
        self.assertEqual(sp.newest_final_with_interpreters(rel), "v0.5.5")
        self.assertIsNone(sp.newest_final_with_interpreters(rel[:3]))


if __name__ == "__main__":
    unittest.main()
