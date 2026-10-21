"""Tests for scripts/release_firmware.py: the cell table and the checks
`assemble` makes on what the builds hand over. Nothing is built.

    python3 -m unittest test_release_firmware -v

Each check is shown to fail by planting its fault in a set that passes.
"""
import hashlib
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "scripts"))
import release_firmware as rf

COMMIT = "a" * 40


def put(d, name, data):
    (d / name).write_bytes(data)
    return {"file": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def fake_cell(root, c, version="v1.30.0-preview"):
    """A cell directory as `build` leaves it, with files that pass every check."""
    d = Path(root) / c["id"]
    d.mkdir(parents=True)
    pad = b"\xff" * 4096
    app = b"\xe9" + bytes(range(256)) * 5000      # 1.28 MB
    files = {}
    if c["kind"] in ("esp32", "espressif"):
        boot = rf.BOOTLOADER[c["chip"]] - c["offset"]
        merged = b"\xff" * boot + b"\xe9" + b"\x00" * (0x10000 - boot - 1) + app + pad
        files["flash"] = put(d, "flash.bin", merged)
        files["app"] = put(d, "app.bin", app)
        files["partitions"] = put(d, "partitions.bin", b"\xaaP" + b"\x00" * 100) if c["kind"] == "esp32" else None
        if files["partitions"] is None:
            del files["partitions"]
    else:
        block = b"UF2\n" + b"WQ]\x9e" + b"\x00" * 500 + b"\x30\x6f\xb1\x0a"
        files["uf2"] = put(d, "firmware.uf2", block * 2000)   # 1 MB
    files["elf"] = put(d, "firmware.elf", b"\x7fELF" + b"\x00" * 4096)
    files["map"] = put(d, "firmware.map", b"m" * 4096)
    meta = {k: c[k] for k in ("id", "label", "interpreter", "port", "board", "variant", "chip", "flash", "modules")}
    meta.update(kind=c["kind"], flash_offset=None if c["offset"] is None else hex(c["offset"]),
                flash_bytes=int(c["flash"][:-2]) * rf.MB, files=files, build_seconds=61,
                interpreter_version=version if c["interpreter"] == "micropython" else "11.0.0-alpha.1-dirty",
                module_revisions={"pydevices": {"commit": COMMIT, "dirty": False}})
    (d / "cell.json").write_text(json.dumps(meta))
    return d


def fake_set(root, pico2=False):
    for c in rf.wanted(pico2):
        fake_cell(root, c)


class Table(unittest.TestCase):
    def test_nine_cells_by_default_and_the_pico2_only_when_asked(self):
        self.assertEqual(len(rf.matrix(False)["include"]), 9)
        ids = [c["id"] for c in rf.matrix(True)["include"]]
        self.assertEqual(len(ids), 10)
        self.assertIn("mp-pico2", ids)
        self.assertNotIn("mp-pico2", [c["id"] for c in rf.matrix(False)["include"]])
        self.assertEqual(len(set(ids)), len(ids))

    def test_five_micropython_and_four_circuitpython(self):
        rows = rf.wanted(False)
        self.assertEqual(sum(c["interpreter"] == "micropython" for c in rows), 5)
        self.assertEqual(sum(c["interpreter"] == "circuitpython" for c in rows), 4)

    def test_every_cell_names_its_files(self):
        for c in rf.CELLS:
            self.assertIn((c["interpreter"], c["kind"]), rf.FILES, c["id"])

    def test_build_command(self):
        c = rf.cell("mp-s3-lcd7")
        args = rf.build_args(c, None)
        self.assertIn("--flash", args)
        self.assertEqual(args[args.index("--variant") + 1], "LCD_7")
        self.assertEqual(args[args.index("--modules") + 1], "all")

    def test_circuitpython_needs_the_lvgl_checkout(self):
        c = rf.cell("cp-s3-lcd7")
        with self.assertRaises(rf.Failed):
            rf.build_args(c, None)
        args = rf.build_args(c, "/tmp/ws")
        self.assertIn("--interpreter", args)
        self.assertNotIn("--flash", args)
        self.assertTrue(args[args.index("--modules") + 1].endswith("/tmp/ws/lvgl-circuitpython"))


class Assemble(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        self.indir, self.out = self.root / "in", self.root / "out"
        self.indir.mkdir()
        fake_set(self.indir)

    def run_assemble(self, **kw):
        return rf.assemble(self.indir, self.out, kw.pop("pico2", False), kw.pop("tag", ""), "https://run", "b" * 40, **kw)

    def fails_with(self, text):
        with self.assertRaises(rf.Failed) as cm:
            self.run_assemble()
        self.assertIn(text, str(cm.exception))

    def test_a_good_set_assembles(self):
        doc = self.run_assemble()
        self.assertEqual(len(doc["builds"]), 9)
        self.assertTrue((self.out / "firmware.json").is_file())
        names = {f["name"] for b in doc["builds"] for f in b["files"]}
        for name in names:
            self.assertTrue((self.out / name).is_file(), name)
        self.assertTrue(doc["preview"])  # v1.30.0-preview-160 and CircuitPython alpha: never published

    def test_firmware_json_records_the_pins_and_the_run(self):
        doc = json.loads((self.out / "firmware.json").read_text()) if self.run_assemble() else None
        self.assertEqual(doc["run"], "https://run")
        self.assertEqual(doc["pins"]["UPSTREAM"], (rf.REPO / "UPSTREAM").read_text().strip())
        self.assertIn("esp-idf", doc["pins"]["deps.lock"])
        self.assertIn("omnipython-lib", doc["pins"]["modules.lock"])
        self.assertEqual(doc["pins"]["lvgl-circuitpython"], "b" * 40)
        s3 = next(b for b in doc["builds"] if b["id"] == "mp-s3-lcd7")
        self.assertEqual(s3["flash_offset"], "0x0")
        self.assertEqual(s3["app_offset"], "0x10000")
        p4 = next(b for b in doc["builds"] if b["id"] == "mp-p4-devkit")
        self.assertEqual(p4["flash_offset"], "0x2000")
        cp4 = next(b for b in doc["builds"] if b["id"] == "cp-p4-devkit")
        self.assertEqual(cp4["flash_offset"], "0x0")
        self.assertEqual(cp4["app_offset"], "0x10000")

    def test_a_tagged_release_is_not_a_preview(self):
        shutil.rmtree(self.indir)
        self.indir.mkdir()
        for c in rf.wanted():
            fake_cell(self.indir, c, version="v1.30.0")
            if c["interpreter"] == "circuitpython":
                p = self.indir / c["id"] / "cell.json"
                m = json.loads(p.read_text())
                m["interpreter_version"] = "11.0.0"
                p.write_text(json.dumps(m))
        doc = self.run_assemble(tag="v1.30.0-20261101")
        self.assertFalse(doc["preview"])
        self.assertEqual(doc["tag"], "v1.30.0-20261101")

    def test_default_tag_is_version_and_date(self):
        import datetime
        doc = self.run_assemble(now=datetime.datetime(2026, 11, 1, tzinfo=datetime.timezone.utc))
        self.assertEqual(doc["tag"], "v1.30.0-20261101")

    # -- planted faults: each must be caught --

    def edit_file(self, cell_id, role, fn):
        d = self.indir / cell_id
        meta = json.loads((d / "cell.json").read_text())
        f = d / meta["files"][role]["file"]
        f.write_bytes(fn(f.read_bytes()))

    def test_a_flipped_byte_is_caught_by_the_checksum(self):
        self.edit_file("mp-s3-lcd7", "elf", lambda b: b[:100] + b"X" + b[101:])
        self.fails_with("sha256")

    def test_a_truncated_image_is_caught(self):
        self.edit_file("mp-s3-lcd7", "flash", lambda b: b[:-1000])
        self.fails_with("mp-s3-lcd7")

    def test_an_empty_file_is_caught(self):
        self.edit_file("cp-twatch", "map", lambda b: b"")
        self.fails_with("is empty")

    def test_a_missing_cell_is_caught(self):
        shutil.rmtree(self.indir / "mp-p4-panel")
        self.fails_with("mp-p4-panel: not built")

    def test_a_pico2_asked_for_but_not_built_is_caught(self):
        with self.assertRaises(rf.Failed) as cm:
            self.run_assemble(pico2=True)
        self.assertIn("mp-pico2: not built", str(cm.exception))

    def test_a_flash_image_not_starting_with_the_magic_is_caught(self):
        def rewrite(b):
            return b"\x00" + b[1:]
        # keep checksums honest: rewrite the recorded sha too
        d = self.indir / "mp-s3-lcd43"
        meta = json.loads((d / "cell.json").read_text())
        f = d / meta["files"]["flash"]["file"]
        f.write_bytes(rewrite(f.read_bytes()))
        meta["files"]["flash"]["sha256"] = rf.sha256(f)
        (d / "cell.json").write_text(json.dumps(meta))
        self.fails_with("magic 0xE9")

    def test_an_app_that_is_not_in_the_flash_image_is_caught(self):
        d = self.indir / "mp-s3-tembed"
        meta = json.loads((d / "cell.json").read_text())
        f = d / meta["files"]["app"]["file"]
        f.write_bytes(b"\xe9" + b"\x01" * 100000)
        meta["files"]["app"] = {"file": f.name, "size": f.stat().st_size, "sha256": rf.sha256(f)}
        (d / "cell.json").write_text(json.dumps(meta))
        self.fails_with("app image is not inside")

    def test_an_image_past_its_flash_is_caught(self):
        d = self.indir / "mp-p4-devkit"
        meta = json.loads((d / "cell.json").read_text())
        meta["flash_bytes"] = 1 * rf.MB
        (d / "cell.json").write_text(json.dumps(meta))
        self.fails_with("past the 16MB flash")

    def test_a_uf2_cut_off_mid_block_is_caught(self):
        d = self.indir / "cp-feather-rp2040"
        meta = json.loads((d / "cell.json").read_text())
        f = d / meta["files"]["uf2"]["file"]
        f.write_bytes(f.read_bytes()[:-100])
        meta["files"]["uf2"]["sha256"] = rf.sha256(f)
        meta["files"]["uf2"]["size"] = f.stat().st_size
        (d / "cell.json").write_text(json.dumps(meta))
        self.fails_with("whole 512-byte blocks")

    def test_an_elf_that_is_not_one_is_caught(self):
        self.edit_file("cp-s3-lcd7", "elf", lambda b: b"NOPE" + b[4:])
        d = self.indir / "cp-s3-lcd7"
        meta = json.loads((d / "cell.json").read_text())
        meta["files"]["elf"]["sha256"] = rf.sha256(d / meta["files"]["elf"]["file"])
        (d / "cell.json").write_text(json.dumps(meta))
        self.fails_with("not an ELF")

    def test_a_dirty_module_is_caught(self):
        d = self.indir / "mp-s3-lcd7"
        meta = json.loads((d / "cell.json").read_text())
        meta["module_revisions"]["pydevices"]["dirty"] = True
        (d / "cell.json").write_text(json.dumps(meta))
        self.fails_with("dirty")

    def test_a_cell_built_for_another_board_is_caught(self):
        d = self.indir / "mp-s3-lcd7"
        meta = json.loads((d / "cell.json").read_text())
        meta["variant"] = "T_EMBED"
        (d / "cell.json").write_text(json.dumps(meta))
        self.fails_with("variant")

    def test_micropython_cells_that_disagree_are_caught(self):
        d = self.indir / "mp-s3-lcd7"
        meta = json.loads((d / "cell.json").read_text())
        meta["interpreter_version"] = "v1.29.0"
        (d / "cell.json").write_text(json.dumps(meta))
        with self.assertRaises(rf.Failed) as cm:
            self.run_assemble()
        self.assertIn("disagree", str(cm.exception))

    def test_a_stray_directory_is_caught(self):
        (self.indir / "mp-extra").mkdir()
        self.fails_with("no cell")


class VersionTag(unittest.TestCase):
    """An image that prints a commit hash instead of a version must not pass."""

    def elf(self, body):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        f = Path(d) / "elf.elf"
        f.write_bytes(body)
        return f

    def test_the_tag_in_the_image_passes(self):
        rf.check_version_tag("v1.30.0-preview.160.gb70ae1aca6", self.elf(b"\0MicroPython v1.30.0-preview.160.gb70ae1aca6 on \0"), "x")

    def test_a_build_that_recorded_no_version_fails(self):
        with self.assertRaisesRegex(rf.Failed, "no version"):
            rf.check_version_tag(None, self.elf(b"MicroPython 2818d7fb1d-dirty"), "x")

    def test_an_image_with_a_commit_instead_fails(self):
        with self.assertRaisesRegex(rf.Failed, "does not contain"):
            rf.check_version_tag("v1.30.0-preview.160.gb70ae1aca6", self.elf(b"MicroPython 2818d7fb1d-dirty on"), "x")


class FrozenLibraries(unittest.TestCase):
    """A CircuitPython image without asyncio and adafruit_ticks must not pass."""

    LIBS = ["Adafruit_CircuitPython_asyncio 3.1.1", "Adafruit_CircuitPython_Ticks 1.1.7"]

    def elf(self, body):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        f = Path(d) / "elf.elf"
        f.write_bytes(body)
        return f

    def test_an_image_with_both_passes(self):
        rf.check_frozen_libraries(self.LIBS, self.elf(b"..asyncio/core.py..adafruit_ticks.py.."), "x")

    def test_a_record_with_no_libraries_fails(self):
        for libs in (None, [], self.LIBS[:1], self.LIBS[1:]):
            with self.assertRaisesRegex(rf.Failed, "names no frozen"):
                rf.check_frozen_libraries(libs, self.elf(b"asyncio/core.py adafruit_ticks.py"), "x")

    def test_an_image_without_the_files_fails(self):
        for body in (b"adafruit_ticks.py", b"asyncio/core.py", b""):
            with self.assertRaisesRegex(rf.Failed, "does not contain"):
                rf.check_frozen_libraries(self.LIBS, self.elf(body), "x")


class DraftRelease(unittest.TestCase):
    """The draft Release's tag rule and notes. A preview set may only be staged."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        indir = self.root / "in"
        indir.mkdir()
        fake_set(indir)
        self.doc = rf.assemble(indir, self.root / "out", False, "staged-20261020", "https://run", "b" * 40)

    def test_a_preview_set_takes_only_a_staged_tag(self):
        rf.check_draft_tag(self.doc)
        self.doc["tag"] = "v1.30.0-20261020"
        with self.assertRaises(rf.Failed) as cm:
            rf.check_draft_tag(self.doc)
        self.assertIn("preview pins", str(cm.exception))

    def test_a_final_set_takes_a_dated_version_or_a_staged_tag(self):
        self.doc.update(preview=False, preview_reasons=[])
        for tag in ("v1.30.0-20261101", "staged-1"):
            self.doc["tag"] = tag
            rf.check_draft_tag(self.doc)
        for tag in ("v1.30.0", "1.30.0-20261101", "latest", "v1.30.0-2026"):
            self.doc["tag"] = tag
            with self.assertRaises(rf.Failed, msg=tag):
                rf.check_draft_tag(self.doc)

    def test_the_notes_warn_name_every_board_and_say_how_to_verify(self):
        text = rf.notes(self.doc)
        self.assertIn("Staged, not for release", text)
        self.assertIn("secrets.py", text)
        self.assertIn("gh attestation verify", text)
        for b in self.doc["builds"]:
            self.assertIn(b["label"], text)
        self.assertIn("esptool --chip esp32p4 write-flash 0x2000 omnipython-mp-esp32_generic_p4-wifi6_dev_kit_h2-staged-20261020.bin", text)
        self.assertIn("--chip esp32s3 write-flash 0x0 ", text)
        self.assertIn("BOOTSEL drive", text)

    def test_a_final_set_has_no_staged_banner(self):
        self.doc.update(preview=False, preview_reasons=[], tag="v1.30.0-20261101")
        self.assertNotIn("Staged", rf.notes(self.doc))

    def test_the_command_refuses_a_bad_tag_and_writes_notes_for_a_good_one(self):
        j = self.root / "out" / "firmware.json"
        out = self.root / "notes.md"
        self.assertEqual(rf.main(["notes", str(j), "--out", str(out)]), 0)
        self.assertIn("OmniPython firmware staged-20261020", out.read_text())
        bad = json.loads(j.read_text())
        bad["tag"] = "v1.30.0-20261020"
        j.write_text(json.dumps(bad))
        out.unlink()
        self.assertEqual(rf.main(["notes", str(j), "--out", str(out)]), 1)
        self.assertFalse(out.exists())


if __name__ == "__main__":
    unittest.main()
