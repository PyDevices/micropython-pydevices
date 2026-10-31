"""scripts/ci_scope.py: which jobs of the firmware workflows a change can reach."""

import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE / "scripts"))
import ci_scope  # noqa: E402

SOURCE = (HERE / ci_scope.FIRMWARE).read_text()


def with_cell(source, cell_id, old, new):
    """The script's text with one cell's field changed."""
    start = source.index('id="%s"' % cell_id)
    end = source.index("dict(id=", start) if "dict(id=" in source[start:] else len(source)
    block = source[start:end]
    assert old in block, (cell_id, old)
    return source[:start] + block.replace(old, new, 1) + source[end:]


class CircuitPythonJobs(unittest.TestCase):
    def reach(self, *files):
        return set().union(*(ci_scope.cp_reach(f) for f in files))

    def test_a_module_reaches_the_jobs_that_build_it(self):
        self.assertEqual(self.reach("modules/pngio/src/pngio.c"), {"unix_modules"})
        self.assertEqual(self.reach("modules/usbhost/src/host.c"), {"unix_modules", "usbhost"})

    def test_a_module_the_jobs_do_not_name_reaches_none(self):
        self.assertEqual(self.reach("modules/audioif/src/audioif.c", "modules/tflite/src/a.c"), set())

    def test_docs_examples_and_the_other_interpreters_patches_reach_none(self):
        self.assertEqual(self.reach("modules/pngio/README.md", "modules/mdns/examples/a.py",
                                    "modules/displayif/docs/x.md", "patches/micropython/0001-x.patch",
                                    "patches/tinyusb/0001-x.patch", "UPSTREAM", "variants/LCD_7/x.cmake",
                                    "docs/a.md", "tests/test_release_firmware.py"), set())

    def test_usbhost_tests_do_not_build_the_p4_image(self):
        self.assertEqual(self.reach("modules/usbhost/tests/test_a.py"), {"unix_modules"})

    def test_the_pins_the_builder_and_circuitpythons_patches_reach_all(self):
        everything = set(ci_scope.CP_JOBS)
        for path in ("build_mp.py", "deps.lock", "modules.lock", "patches/circuitpython/0001-x.patch",
                     "modules/library.py", "modules/manifest.py", "a_path_no_rule_names.txt"):
            self.assertEqual(self.reach(path), everything, path)

    def test_the_workflow_reaches_all_and_another_workflow_none(self):
        self.assertEqual(self.reach(".github/workflows/circuitpython.yml"), set(ci_scope.CP_JOBS))
        self.assertEqual(self.reach(".github/workflows/jpegio.yml"), set())


class FirmwareCells(unittest.TestCase):
    def scope(self, files, head=SOURCE, base=SOURCE):
        return ci_scope.firmware_scope(files, lambda: base, lambda: head)

    def test_the_cells_parse(self):
        cells, rest = ci_scope.cells_of(SOURCE)
        self.assertEqual(len(cells), 12)
        self.assertIn("cp-twatch", cells)
        self.assertTrue(rest)

    def test_the_stager_and_the_tests_build_nothing(self):
        self.assertEqual(self.scope(["scripts/stage_firmware_page.py", "tests/test_stage_firmware_page.py",
                                     "tests/test_release_firmware.py"]), (False, False, []))

    def test_the_workflow_builds_the_whole_set(self):
        self.assertEqual(self.scope([ci_scope.FIRMWARE_WORKFLOW]), (True, True, []))

    def test_one_cells_row_builds_that_cell(self):
        head = with_cell(SOURCE, "mp-s3-lcd43", 'flash="8MB"', 'flash="16MB"')
        self.assertEqual(self.scope([ci_scope.FIRMWARE], head), (True, False, ["mp-s3-lcd43"]))

    def test_two_rows_build_two_cells(self):
        head = with_cell(SOURCE, "mp-s3-lcd43", 'flash="8MB"', 'flash="16MB"')
        head = with_cell(head, "cp-twatch", 'flash="16MB"', 'flash="8MB"')
        self.assertEqual(self.scope([ci_scope.FIRMWARE], head), (True, False, ["cp-twatch", "mp-s3-lcd43"]))

    def test_code_outside_cells_builds_the_whole_set(self):
        head = SOURCE.replace("class Failed(Exception):", "MORE = 1\n\n\nclass Failed(Exception):", 1)
        self.assertEqual(self.scope([ci_scope.FIRMWARE], head), (True, True, []))

    def test_a_cell_added_or_removed_builds_the_whole_set(self):
        head = re.sub(r'    dict\(id="cp-twatch".*?kind="espressif", deps="cp-esp"\),\n', "", SOURCE, flags=re.S)
        self.assertNotEqual(head, SOURCE)
        self.assertEqual(self.scope([ci_scope.FIRMWARE], head), (True, True, []))

    def test_an_off_by_default_cell_builds_nothing(self):
        head = with_cell(SOURCE, "mp-pico2", 'flash="4MB"', 'flash="8MB"')
        self.assertEqual(self.scope([ci_scope.FIRMWARE], head), (False, False, []))


if __name__ == "__main__":
    unittest.main()
