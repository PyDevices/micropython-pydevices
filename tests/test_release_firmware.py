# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Tests for scripts/release_firmware.py: the cell table and the checks
`assemble` makes on what the builds hand over. Nothing is built.

    PYTHONPATH=tests python3 -m unittest test_release_firmware -v

Each check is shown to fail by planting its fault in a set that passes.
"""
import contextlib
import hashlib
import io
import json
import shutil
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import release_firmware as rf

COMMIT = "a" * 40


def put(d, name, data):
    (d / name).write_bytes(data)
    return {"file": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def table_bin(rows):
    """A binary ESP32 partition table: (name, type, subtype, offset, size) rows."""
    return b"".join(struct.pack("<2sBBII16sI", b"\xaa\x50", t, st, off, size, name.encode(), 0)
                    for name, t, st, off, size in rows) + b"\xff" * 64


# What an autosize cell's filesystem comes to after build_mp.py grew the app slot.
AUTOSIZED_FS = 2624 * 1024


def kept_table(c):
    """The table a cell's build makes: the app partition up to the filesystem it keeps,
    or up to the one an autosize cell was left with."""
    flash, fs = int(c["flash"][:-2]) * rf.MB, rf.size_bytes(c.get("fs")) or AUTOSIZED_FS
    return table_bin([("nvs", 1, 2, 0x9000, 0x6000), ("phy_init", 1, 1, 0xF000, 0x1000),
                      ("factory", 0, 0, 0x10000, flash - fs - 0x10000)])


# What a GIGA build moves to its ROMFS, cut down.
ROMFS_MODULES = ["asyncio/core.mpy", "webrepl.mpy"]


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
        files["partitions"] = put(d, "partitions.bin", kept_table(c)) if c["kind"] == "esp32" else None
        if files["partitions"] is None:
            del files["partitions"]
    elif c["kind"] == "stm32":
        # A vector table at the image's start: a stack in RAM, then a Thumb reset address inside it.
        head = struct.pack("<II", 0x24080000, c["offset"] + 0x2001)
        files["flash"] = put(d, "firmware0.bin", head + b"\x00" * (1536 * 1024))
        files["wifi"] = put(d, "firmware1.bin", b"w" * 400000)
        files["romfs"] = put(d, "romfs.img", rf.ROMFS_HEADER + b"\x80\x01" + b"core.mpy webrepl.mpy" + b"r" * 30000)
        zipped = io.BytesIO()
        with zipfile.ZipFile(zipped, "w") as z:
            for name in ROMFS_MODULES:
                z.writestr(name, b"M")
        files["romfs-files"] = put(d, "romfs-files.zip", zipped.getvalue())
    else:
        block = b"UF2\n" + b"WQ]\x9e" + b"\x00" * 500 + b"\x30\x6f\xb1\x0a"
        files["uf2"] = put(d, "firmware.uf2", block * 2000)   # 1 MB
    files["elf"] = put(d, "firmware.elf", b"\x7fELF" + b"\x00" * 4096)
    files["map"] = put(d, "firmware.map", b"m" * 4096)
    meta = {k: c[k] for k in ("id", "label", "interpreter", "port", "board", "variant", "chip", "flash", "modules")}
    if c["kind"] == "stm32":
        meta["wifi_offset"] = rf.hex8(rf.WIFI_OFFSET)
        meta["romfs_offset"], meta["romfs_modules"] = rf.hex8(rf.ROMFS_OFFSET), ROMFS_MODULES
    meta.update(kind=c["kind"], flash_offset=None if c["offset"] is None else rf.hex8(c["offset"]) if c["kind"] == "stm32" else hex(c["offset"]),
                flash_bytes=int(c["flash"][:-2]) * rf.MB, files=files, build_seconds=61,
                interpreter_version=version if c["interpreter"] == "micropython" else "11.0.0-alpha.1-dirty",
                module_revisions={"pydevices": {"commit": COMMIT, "dirty": False}})
    if c["kind"] == "rp2" and c.get("fs"):
        meta["rp2_storage_bytes"] = rf.size_bytes(c["fs"])
    (d / "cell.json").write_text(json.dumps(meta))
    return d


def fake_set(root, pico2=False):
    for c in rf.wanted(pico2):
        fake_cell(root, c)


class Table(unittest.TestCase):
    def test_sixteen_cells_by_default_and_the_pico2_only_when_asked(self):
        self.assertEqual(len(rf.matrix(False)["include"]), 16)
        ids = [c["id"] for c in rf.matrix(True)["include"]]
        self.assertEqual(len(ids), 18)
        self.assertIn("mp-pico2", ids)
        self.assertIn("mp-pico-plus2-w", ids)
        self.assertNotIn("mp-pico2", [c["id"] for c in rf.matrix(False)["include"]])
        self.assertNotIn("mp-pico-plus2-w", [c["id"] for c in rf.matrix(False)["include"]])
        self.assertEqual(len(set(ids)), len(ids))

    def test_ten_micropython_and_six_circuitpython(self):
        rows = rf.wanted(False)
        self.assertEqual(sum(c["interpreter"] == "micropython" for c in rows), 10)
        self.assertEqual(sum(c["interpreter"] == "circuitpython" for c in rows), 6)

    def test_the_generic_s3_cells_fit_the_boards_they_name(self):
        by = {c["id"]: c for c in rf.CELLS}
        self.assertEqual((by["mp-s3-octal"]["variant"], by["mp-s3-octal"]["flash"]), ("SPIRAM_OCT", "16MB"))
        self.assertEqual((by["mp-s3"]["variant"], by["mp-s3"]["flash"]), (None, "8MB"))
        self.assertEqual((by["mp-s3-8mb"]["variant"], by["mp-s3-8mb"]["flash"]), ("NO_PSRAM", "8MB"))
        self.assertEqual(by["cp-s3-qualia"]["board"], "adafruit_qualia_s3_rgb666_single_app")
        for cid, board in (("mp-s3-octal", "Qualia"), ("mp-s3", "QT Py"), ("mp-s3-8mb", "QT Py"), ("cp-s3-qualia", "Qualia")):
            self.assertIn(board, by[cid]["label"])

    def test_no_cell_builds_picotts(self):
        # picotts is opt-in: `all` leaves it out, so a cell gets it only by naming it.
        for c in rf.CELLS:
            self.assertNotIn("picotts", c["modules"].split(","), c["id"])

    def test_every_cell_names_its_files(self):
        for c in rf.CELLS:
            self.assertIn((c["interpreter"], c["kind"]), rf.FILES, c["id"])

    def test_build_command(self):
        c = rf.cell("mp-s3-lcd7")
        args = rf.build_args(c)
        self.assertIn("--flash", args)
        self.assertEqual(args[args.index("--variant") + 1], "LCD_7")
        self.assertEqual(args[args.index("--modules") + 1], "all")

    def test_the_p4_cells_carry_the_opt_in_ml_modules(self):
        for cell_id in ("mp-p4-devkit", "mp-p4-panel", "mp-p4-eye"):
            args = rf.build_args(rf.cell(cell_id))
            self.assertEqual(args[args.index("--modules") + 1], "all,vision,tflite", cell_id)

    def test_every_micropython_esp32_and_rp2_cell_keeps_its_filesystem(self):
        rf.check_cells()
        kept = [c["id"] for c in rf.CELLS if c.get("fs")]
        self.assertEqual(len(kept), 5)
        self.assertEqual([c["id"] for c in rf.CELLS if c.get("autosize")], ["mp-s3-lcd7", "mp-s3-lcd43", "mp-s3-octal", "mp-s3", "mp-s3-8mb", "mp-s3-tembed"])
        for c in rf.CELLS:
            args = rf.build_args(c)
            if c.get("fs"):
                self.assertEqual(args[args.index("--keep-fs") + 1], c["fs"], c["id"])
            else:
                self.assertNotIn("--keep-fs", args, c["id"])

    def test_a_cell_that_names_no_filesystem_is_caught(self):
        for planted in (None, "2690K", "big"):
            cells = [dict(c) for c in rf.CELLS]
            next(c for c in cells if c["id"] == "mp-p4-panel")["fs"] = planted
            with mock.patch.object(rf, "CELLS", cells):
                with self.assertRaises(rf.Failed) as cm:
                    rf.matrix(False)
                self.assertIn("mp-p4-panel: names no filesystem to keep", str(cm.exception))

    def test_an_autosize_cell_that_also_names_a_filesystem_is_caught(self):
        cells = [dict(c, fs="2688K") if c["id"] == "mp-s3-lcd7" else c for c in rf.CELLS]
        with mock.patch.object(rf, "CELLS", cells):
            with self.assertRaisesRegex(rf.Failed, "mp-s3-lcd7: autosize is for an esp32 cell that names no fs"):
                rf.check_cells()

    def test_a_filesystem_on_a_cell_that_cannot_keep_one_is_caught(self):
        cells = [dict(c, fs="1536K") if c["id"] == "cp-feather-rp2040" else c for c in rf.CELLS]
        with mock.patch.object(rf, "CELLS", cells):
            with self.assertRaisesRegex(rf.Failed, "cp-feather-rp2040: fs is for"):
                rf.check_cells()

    def test_circuitpython_names_lvgl_like_micropython(self):
        c = rf.cell("cp-s3-lcd7")
        args = rf.build_args(c)
        self.assertIn("--interpreter", args)
        self.assertNotIn("--flash", args)
        self.assertEqual(args[args.index("--modules") + 1].split(",")[-1], "lvgl")


class BuildTheGiga(unittest.TestCase):
    """`build` hands over the GIGA's ROMFS image, and the same modules as a zip."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.src = self.root / "src"
        (self.src / "romfs" / "asyncio").mkdir(parents=True)
        (self.src / "romfs" / "asyncio" / "core.mpy").write_bytes(b"M1")
        (self.src / "romfs" / "webrepl.mpy").write_bytes(b"M2")
        for name, data in (("firmware0.bin", b"0" * 64), ("firmware1.bin", b"1" * 64), ("romfs.img", b"r" * 64),
                           ("firmware.elf", b"\x7fELF v1.30.0-preview"), ("firmware.map", b"m")):
            (self.src / name).write_bytes(data)
        (self.src / "pydevices-build.json").write_text(json.dumps(
            {"complete": True, "micropython_version": "v1.30.0-preview", "romfs_modules": ROMFS_MODULES}))

    def tearDown(self):
        shutil.rmtree(self.root)

    def test_the_romfs_image_and_zip_go_with_firmware0(self):
        with mock.patch.object(rf, "build_dir", return_value=self.src), \
                mock.patch.object(rf, "micropython_version", return_value="v1.30.0-preview"), \
                mock.patch.object(rf.subprocess, "run", return_value=mock.Mock(returncode=0)), \
                contextlib.redirect_stdout(io.StringIO()):
            rf.build("mp-giga-shield", self.root / "out")
        out = self.root / "out" / "mp-giga-shield"
        meta = json.loads((out / "cell.json").read_text())
        self.assertEqual(sorted(meta["files"]), ["elf", "flash", "map", "romfs", "romfs-files", "wifi"])
        self.assertEqual((out / "romfs.img").read_bytes(), b"r" * 64)
        with zipfile.ZipFile(out / "romfs-files.zip") as z:
            self.assertEqual(z.read("asyncio/core.mpy"), b"M1")
            self.assertEqual(z.read("webrepl.mpy"), b"M2")
        self.assertEqual((meta["romfs_offset"], meta["romfs_modules"]), ("0x90B00000", ROMFS_MODULES))


class Assemble(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        self.indir, self.out = self.root / "in", self.root / "out"
        self.indir.mkdir()
        fake_set(self.indir)

    def run_assemble(self, **kw):
        return rf.assemble(self.indir, self.out, kw.pop("pico2", False), kw.pop("tag", ""), "https://run", **kw)

    def fails_with(self, text):
        with self.assertRaises(rf.Failed) as cm:
            self.run_assemble()
        self.assertIn(text, str(cm.exception))

    def test_a_good_set_assembles(self):
        doc = self.run_assemble()
        self.assertEqual(len(doc["builds"]), 16)
        self.assertTrue((self.out / "firmware.json").is_file())
        names = {f["name"] for b in doc["builds"] for f in b["files"]}
        for name in names:
            self.assertTrue((self.out / name).is_file(), name)
        self.assertTrue(doc["preview"])  # v1.30.0-preview-160 and CircuitPython alpha: never published

    def test_firmware_json_records_the_pins_and_the_run(self):
        doc = json.loads((self.out / "firmware.json").read_text()) if self.run_assemble() else None
        self.assertEqual(doc["run"], "https://run")
        lock = rf.pinfile.read(rf.REPO)
        self.assertEqual(doc["pins"]["micropython"], {"commit": lock["micropython"].ref})
        self.assertEqual(doc["pins"]["esp-idf"], {"commit": lock["esp-idf"].ref, "tag": lock["esp-idf"].tag})
        self.assertEqual(doc["pins"]["SDL2"], {"version": lock["SDL2"].ref})
        self.assertEqual(set(doc["pins"]), set(lock))
        s3 = next(b for b in doc["builds"] if b["id"] == "mp-s3-lcd7")
        self.assertEqual(s3["flash_offset"], "0x0")
        self.assertEqual(s3["app_offset"], "0x10000")
        p4 = next(b for b in doc["builds"] if b["id"] == "mp-p4-devkit")
        self.assertEqual(p4["flash_offset"], "0x2000")
        cp4 = next(b for b in doc["builds"] if b["id"] == "cp-p4-devkit")
        self.assertEqual(cp4["flash_offset"], "0x0")
        self.assertEqual(cp4["app_offset"], "0x10000")

    def test_the_giga_ships_three_images_named_for_where_they_go(self):
        doc = self.run_assemble(tag="v1.30.0-20261106")
        giga = next(b for b in doc["builds"] if b["id"] == "mp-giga-shield")
        names = {f["role"]: f["name"] for f in giga["files"]}
        stem = "omnipython-mp-arduino_giga-display_shield-v1.30.0-20261106"
        self.assertEqual(names["flash"], stem + ".firmware0-at-0x08040000.bin")
        self.assertEqual(names["wifi"], stem + ".firmware1-at-0x90F00000.bin")
        self.assertEqual(names["romfs"], stem + ".romfs-at-0x90B00000.img")
        self.assertEqual(names["romfs-files"], stem + ".romfs-files.zip")
        self.assertEqual((giga["flash_offset"], giga["wifi_offset"], giga["romfs_offset"]),
                         ("0x08040000", "0x90F00000", "0x90B00000"))
        self.assertEqual(giga["romfs_modules"], ROMFS_MODULES)
        for name in names.values():
            self.assertTrue((self.out / name).is_file(), name)

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
        d = self.indir / "mp-p4-panel"
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

    def rewrite_giga(self, role, fn):
        d = self.indir / "mp-giga-shield"
        meta = json.loads((d / "cell.json").read_text())
        f = d / meta["files"][role]["file"]
        f.write_bytes(fn(f.read_bytes()))
        meta["files"][role].update(size=f.stat().st_size, sha256=rf.sha256(f))
        (d / "cell.json").write_text(json.dumps(meta))

    def test_a_giga_image_without_a_vector_table_is_caught(self):
        self.rewrite_giga("flash", lambda b: b"\xff" * 8 + b[8:])
        self.fails_with("does not start with a vector table")

    def test_a_giga_image_that_expects_another_address_is_caught(self):
        self.rewrite_giga("flash", lambda b: struct.pack("<II", 0x24080000, 0x08000001) + b[8:])
        self.fails_with("does not start with a vector table")

    def test_a_giga_image_past_the_flash_is_caught(self):
        self.rewrite_giga("flash", lambda b: b + b"\x00" * (1 * rf.MB))
        self.fails_with("past the 2MB flash")

    def test_a_giga_wifi_image_past_the_qspi_flash_is_caught(self):
        self.rewrite_giga("wifi", lambda b: b + b"w" * (1 * rf.MB))
        self.fails_with("past the QSPI flash")

    def test_a_giga_without_its_romfs_image_is_caught(self):
        d = self.indir / "mp-giga-shield"
        meta = json.loads((d / "cell.json").read_text())
        del meta["files"]["romfs"]
        (d / "cell.json").write_text(json.dumps(meta))
        self.fails_with("mp-giga-shield: no romfs file")

    def test_a_giga_build_that_moved_no_python_is_caught(self):
        d = self.indir / "mp-giga-shield"
        meta = json.loads((d / "cell.json").read_text())
        meta["romfs_modules"] = []
        (d / "cell.json").write_text(json.dumps(meta))
        self.fails_with("moved no Python modules to ROMFS")

    def test_a_giga_romfs_image_that_is_not_one_is_caught(self):
        self.rewrite_giga("romfs", lambda b: b"\xff" * 3 + b[3:])
        self.fails_with("does not start with a ROMFS header")

    def test_a_giga_romfs_image_past_its_partition_is_caught(self):
        self.rewrite_giga("romfs", lambda b: b + b"r" * (4 * rf.MB))
        self.fails_with("more than the ROMFS partition holds")

    def test_a_giga_romfs_image_without_a_module_is_caught(self):
        self.rewrite_giga("romfs", lambda b: b.replace(b"webrepl.mpy", b"other.mpy"))
        self.fails_with("lacks webrepl.mpy")

    def test_a_giga_romfs_zip_without_a_module_is_caught(self):
        def drop(blob):
            out = io.BytesIO()
            with zipfile.ZipFile(io.BytesIO(blob)) as src, zipfile.ZipFile(out, "w") as z:
                for name in src.namelist():
                    if name != "asyncio/core.mpy":
                        z.writestr(name, src.read(name))
            return out.getvalue()
        self.rewrite_giga("romfs-files", drop)
        self.fails_with("romfs-files.zip lacks asyncio/core.mpy")

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

    def test_firmware_json_says_where_each_filesystem_is(self):
        doc = self.run_assemble()
        fs = {b["id"]: (b.get("fs_offset"), b.get("fs_bytes")) for b in doc["builds"]}
        self.assertEqual(fs["mp-s3-lcd7"], ("0x570000", AUTOSIZED_FS))
        self.assertEqual(fs["mp-s3-lcd43"], ("0x570000", AUTOSIZED_FS))
        self.assertEqual(fs["mp-p4-panel"], ("0x910000", 23488 * 1024))
        self.assertEqual(fs["mp-p4-eye"], ("0x910000", 7104 * 1024))
        self.assertEqual(fs["cp-s3-lcd7"], (None, None))

    def rewrite(self, cell_id, role, data):
        d = self.indir / cell_id
        meta = json.loads((d / "cell.json").read_text())
        f = d / meta["files"][role]["file"]
        f.write_bytes(data)
        meta["files"][role].update(size=len(data), sha256=rf.sha256(f))
        (d / "cell.json").write_text(json.dumps(meta))

    def test_an_autosize_cell_may_grow_its_app_and_move_its_filesystem(self):
        self.rewrite("mp-s3-lcd7", "partitions", table_bin([
            ("nvs", 1, 2, 0x9000, 0x6000), ("phy_init", 1, 1, 0xF000, 0x1000),
            ("factory", 0, 0, 0x10000, 0x550000), ("coredump", 1, 3, 0x560000, 0x10000)]))
        doc = self.run_assemble()
        lcd7 = next(b for b in doc["builds"] if b["id"] == "mp-s3-lcd7")
        self.assertEqual((lcd7["fs_offset"], lcd7["fs_bytes"]), ("0x570000", 2624 * 1024))

    def test_a_filesystem_that_moved_is_caught(self):
        # autosize's table for a bigger image: the app grown by 64 KB, the filesystem after it.
        self.rewrite("mp-p4-panel", "partitions", table_bin([
            ("nvs", 1, 2, 0x9000, 0x6000), ("phy_init", 1, 1, 0xF000, 0x1000),
            ("factory", 0, 0, 0x10000, 0x910000), ("coredump", 1, 3, 0x920000, 0x10000)]))
        self.fails_with("mp-p4-panel: the filesystem is 23360 KB at 0x930000, not the 23488K at 0x910000")

    def test_a_named_filesystem_in_the_wrong_place_is_caught(self):
        self.rewrite("mp-p4-devkit", "partitions", table_bin([
            ("nvs", 1, 2, 0x9000, 0x6000), ("factory", 0, 0, 0x10000, 0x500000),
            ("vfs", 1, 0x81, 0x510000, 0xAF0000)]))
        self.fails_with("mp-p4-devkit: the filesystem is 11200 KB at 0x510000")

    def test_a_flash_image_that_runs_into_the_filesystem_is_caught(self):
        d = self.indir / "mp-p4-panel"
        meta = json.loads((d / "cell.json").read_text())
        flash = (d / meta["files"]["flash"]["file"]).read_bytes()
        self.rewrite("mp-p4-panel", "flash", flash + b"\xff" * (0x910000 - len(flash) + 1))
        self.fails_with("mp-p4-panel: flash.bin writes past 0x910000, into the filesystem")

    def pico2_set(self):
        for c in rf.wanted(True):
            if c.get("optional"):
                fake_cell(self.indir, c)
        return self.indir / "mp-pico2"

    @staticmethod
    def uf2_block(addr, family=0xE48BFF59):
        head = struct.pack("<IIIIIIII", 0x0A324655, 0x9E5D5157, 0x2000, addr, 256, 0, 1, family)
        return head + b"\x00" * 476 + struct.pack("<I", 0x0AB16F30)

    def test_a_pico2_set_keeps_its_filesystems(self):
        self.pico2_set()
        doc = self.run_assemble(pico2=True)
        pico2 = next(b for b in doc["builds"] if b["id"] == "mp-pico2")
        self.assertEqual((pico2["fs_offset"], pico2["fs_bytes"]), ("0x350000", 704 * 1024))

    def test_an_rp2_filesystem_that_moved_is_caught(self):
        d = self.pico2_set()
        meta = json.loads((d / "cell.json").read_text())
        meta["rp2_storage_bytes"] = 640 * 1024
        (d / "cell.json").write_text(json.dumps(meta))
        with self.assertRaisesRegex(rf.Failed, "mp-pico2: the filesystem is 640 KB at 0x360000, not the 704K"):
            self.run_assemble(pico2=True)

    def test_an_rp2_build_that_recorded_no_filesystem_is_caught(self):
        d = self.pico2_set()
        meta = json.loads((d / "cell.json").read_text())
        del meta["rp2_storage_bytes"]
        (d / "cell.json").write_text(json.dumps(meta))
        with self.assertRaisesRegex(rf.Failed, "mp-pico2: the build recorded no filesystem size"):
            self.run_assemble(pico2=True)

    def test_a_uf2_that_writes_into_the_filesystem_is_caught(self):
        self.pico2_set()
        last = rf.XIP_BASE + 0x350000 - 256
        # The RP2350's errata block past the image is written at an absolute address and doesn't count.
        good = self.uf2_block(rf.XIP_BASE) * 2000 + self.uf2_block(last) + self.uf2_block(0x10FFFF00, rf.UF2_FAMILY_ABSOLUTE)
        self.rewrite("mp-pico2", "uf2", good)
        self.run_assemble(pico2=True)
        self.rewrite("mp-pico2", "uf2", good + self.uf2_block(last + 256))
        with self.assertRaisesRegex(rf.Failed, "mp-pico2: firmware.uf2 writes past 0x10350000, into the filesystem"):
            self.run_assemble(pico2=True)


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


class CircuitpythonVersionTag(unittest.TestCase):
    """A CircuitPython image prints "<version> on <date>"; a -dirty one must not pass for a clean record."""

    def elf(self, body):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        f = Path(d) / "elf.elf"
        f.write_bytes(body)
        return f

    def test_the_clean_version_in_the_banner_passes(self):
        rf.check_version_tag("11.0.0-alpha.1", self.elf(b"\0Adafruit CircuitPython 11.0.0-alpha.1 on 2026-10-21\0"), "x", b" on ")

    def test_an_image_that_printed_dirty_fails(self):
        with self.assertRaisesRegex(rf.Failed, "does not contain"):
            rf.check_version_tag("11.0.0-alpha.1", self.elf(b"Adafruit CircuitPython 11.0.0-alpha.1-dirty on 2026-10-21"), "x", b" on ")

    def test_a_build_that_recorded_no_version_fails(self):
        with self.assertRaisesRegex(rf.Failed, "no version"):
            rf.check_version_tag(None, self.elf(b"CircuitPython 11.0.0-alpha.1 on"), "x", b" on ")


class DraftRelease(unittest.TestCase):
    """The draft Release's tag rule and notes. A preview set may only be staged."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        indir = self.root / "in"
        indir.mkdir()
        fake_set(indir)
        self.doc = rf.assemble(indir, self.root / "out", False, "staged-20261020", "https://run")

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

    def test_the_notes_give_the_giga_three_dfu_commands_wifi_first_firmware0_last(self):
        text = rf.notes(self.doc)
        stem = "omnipython-mp-arduino_giga-display_shield-staged-20261020"
        wifi = f"dfu-util -a 1 -d 2341:0366 -s 0x90F00000 -D {stem}.firmware1-at-0x90F00000.bin"
        romfs = f"dfu-util -a 1 -d 2341:0366 -s 0x90B00000 -D {stem}.romfs-at-0x90B00000.img"
        main = f"dfu-util -a 0 -d 2341:0366 -s 0x08040000:leave -D {stem}.firmware0-at-0x08040000.bin"
        self.assertIn(wifi + "`, then `" + romfs + "`, then `" + main, text)
        self.assertIn("romfs-files", text)
        self.assertIn("flash-a-board.md#stm32-arduino-giga-r1-wifi", text)

    def test_the_notes_give_each_micropython_board_its_filesystem(self):
        text = rf.notes(self.doc)
        self.assertIn("| ESP32-S3-Touch-LCD-7 | MicroPython v1.30.0-preview | ", text)
        self.assertIn(" | 2,624 KB at 0x570000 |", text)
        self.assertIn(" | 23,488 KB at 0x910000 |", text)
        self.assertNotIn("This release moves the filesystem", text)

    def test_a_release_that_moves_a_filesystem_says_which_board(self):
        previous = json.loads(json.dumps(self.doc))
        previous["tag"] = "v1.30.0-20261101"
        self.assertNotIn("moves the filesystem", rf.notes(self.doc, previous))
        lcd7 = next(b for b in previous["builds"] if b["id"] == "mp-s3-lcd7")
        lcd7.update(fs_offset="0x5a0000", fs_bytes=2432 * 1024)
        text = rf.notes(self.doc, previous)
        self.assertIn("**This release moves the filesystem** on the boards below. Updating one from v1.30.0-20261101", text)
        self.assertIn("- ESP32-S3-Touch-LCD-7, MicroPython: 2,624 KB at 0x570000, was 2,432 KB at 0x5a0000", text)
        self.assertNotIn("- ESP32-S3-Touch-LCD-4.3", text)

    def test_the_command_reads_the_previous_set(self):
        j = self.root / "out" / "firmware.json"
        previous = json.loads(j.read_text())
        previous["tag"] = "staged-20261001"
        next(b for b in previous["builds"] if b["id"] == "mp-p4-panel")["fs_bytes"] = 1
        p = self.root / "previous.json"
        p.write_text(json.dumps(previous))
        out = self.root / "notes.md"
        self.assertEqual(rf.main(["notes", str(j), "--previous", str(p), "--out", str(out)]), 0)
        self.assertIn("- ESP32-P4-WIFI6-Touch-LCD-4B (P4 before revision 3), MicroPython: 23,488 KB at 0x910000, was 0 KB at 0x910000",
                      out.read_text())

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
