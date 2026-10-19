# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""The host leg's verdict: a pass means every check ran.

tools/matrix/run_host.py drives real boards, so these tests drive it with a
fake `Board` that answers from a table. They check what the runner decides
from the answers, not the boards.
"""

import contextlib
import io
import json
import pathlib
import sys
import unittest
from unittest import mock

MATRIX = pathlib.Path(__file__).resolve().parents[1] / "tools" / "matrix"
sys.path.insert(0, str(MATRIX))
import run_host  # noqa: E402

GOOD = {
    "check_enumerate": {"devices": [], "errors": 0, "attaches": 1},  # judged by the stand-in below
    "check_msc": {"same": True, "removed": True, "listing": []},
    "check_hid": {"boot_protocol": True, "report_descriptor": 63, "quiet_reads": 10, "reads": 10, "ms": 1000},
}


class FakeBoard:
    """Answers host_board.<call> from GOOD, with `override` replacing a check's answer."""
    override = {}

    def __init__(self, dev):
        self.dev = dev

    def start(self, modules):
        pass

    def stop(self):
        pass

    def run(self, code, timeout=30):
        return 0, ""

    def value(self, code, timeout=60):
        name = code.split(".", 1)[1].split("(")[0]
        return json.dumps(self.override.get(name, GOOD.get(name)))


def passing_enumerate(r, expect):
    return True, {"devices": ["fake"]}


def drive(*argv, override=None):
    """Run main() with the fake board; return (exit code, printed lines)."""
    out = io.StringIO()
    FakeBoard.override = override or {}
    with mock.patch.object(sys, "argv", ["run_host.py", "--no-put", *argv]), \
            mock.patch.object(run_host, "Board", FakeBoard), \
            mock.patch.object(run_host, "judge_enumerate", passing_enumerate), \
            contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
        try:
            run_host.main()
            code = 0
        except SystemExit as e:
            code = e.code
    return code, out.getvalue().strip().splitlines()


class Verdict(unittest.TestCase):
    def test_host_only_pass_says_what_it_left_out(self):
        code, lines = drive("--host", "X", "--host-only")
        self.assertEqual(code, 0)
        self.assertEqual(lines[-1], "HOST LEG PASS 3 of 3 (host-only; 5 need --peer)")

    def test_no_peer_and_no_host_only_stops_before_touching_a_board(self):
        with mock.patch.object(run_host, "Board", side_effect=AssertionError("touched a board")):
            code, lines = drive("--host", "X")
        self.assertEqual(code, 2)
        self.assertEqual(lines, [])

    def test_unexpected_skip_fails(self):
        # The board finds no keyboard: that is a skip nobody asked for.
        code, lines = drive("--host", "X", "--host-only", override={"check_hid": {"skipped": "no keyboard on the port"}})
        self.assertEqual(code, 1)
        self.assertEqual(lines[-1], "HOST LEG FAIL 2 of 3 (host-only; 5 need --peer; unexpected skip: hid)")

    def test_failing_check_fails(self):
        code, lines = drive("--host", "X", "--host-only", override={"check_msc": dict(GOOD["check_msc"], same=False)})
        self.assertEqual(code, 1)
        self.assertTrue(lines[-1].startswith("HOST LEG FAIL 2 of 3"))

    def test_nothing_ran_is_not_a_pass(self):
        good, line = run_host.verdict([], True)
        self.assertFalse(good)

    def test_verdict_with_a_peer_has_no_notes(self):
        rows = [{"check": c, "pass": True} for c in run_host.CHECKS]
        self.assertEqual(run_host.verdict(rows, False), (True, "HOST LEG PASS 8 of 8"))

    def test_skip_with_a_peer_is_unexpected(self):
        rows = [{"check": c, "pass": True} for c in run_host.CHECKS[:-1]] + [{"check": "video", "skipped": "x"}]
        good, line = run_host.verdict(rows, False)
        self.assertFalse(good)
        self.assertEqual(line, "HOST LEG FAIL 7 of 8 (unexpected skip: video)")

    def test_peer_and_host_only_contradict(self):
        code, _ = drive("--host", "X", "--peer", "Y", "--host-only")
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
