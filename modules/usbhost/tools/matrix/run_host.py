"""The proof matrix's host leg: a board's native port as USB host, checked
against what's plugged into it. Runs in WSL; drives the host board (and a
peer board, if one is given) over ``mpftp hold``. One command, one verdict:

    python3 tools/matrix/run_host.py --host COM49 --peer COM31       # LCD-4.3 #1 with a board on its hub: all 8 checks
    python3 tools/matrix/run_host.py --host COM49 --host-only        # no peer: the 3 checks that need none
    python3 tools/matrix/run_host.py --host COM49 --peer COM31 --plant all   # every check shown failing

The host board needs usbhost's firmware, usb-core and the usb-host-*
packages, midiports and adafruit_midi in /lib. The runner copies
``host_board.py`` and the check scripts in ``tools/host/`` to its root
itself (``--no-put`` skips that). The checks, in order:

- enumerate: every device on the port listed with its strings, each
  configuration descriptor sound (``desc_check.py``), and the classes
  ``--expect`` names present (a flash drive and a keyboard by default);
- msc: the flash drive mounted through ``usb.host.msc``, a 16 KB file
  written, read back identical and removed (``msc_mount_check.py``);
- hid: a real keyboard switched to boot protocol and read back, its LEDs
  set, and ten reads with nobody typing each timing out cleanly;
- hid_typed: a peer board types a, b, shift+C through ``usb_hid``
  (``kbd_device.py``) and the host reads the six reports in order
  (``kbd_host.py``);
- cdc: ``print('cdc', 6 * 7)`` typed into the peer's REPL over its USB
  serial port, ``cdc 42`` read back (``cdc_check.py``);
- midi: 200 note-ons through the peer's MIDI echo (``matrix_board.py``),
  all back and in order;
- audio: 3 s of 440 Hz into the peer's ``usb-device-audio`` speaker
  (``examples/usb_soundcard.py``), all of it taken, no bus errors;
- video: 3 s from the peer's ``usb-device-video`` camera
  (``examples/usb_webcam.py``), at least 5 frames a second, the last MJPEG
  frame decoded at the size negotiated.

A peer is a MicroPython board whose device port is on the host's port (the
DEV-KIT's H2 on LCD-4.3 #1's hub) and whose console is ``--peer``. It needs
micropython-lib's usb-device-cdc and -midi, ``usb_hid``, ``adafruit_hid``,
``midiports``, and for audio and video usbhost's ``usb-device-audio`` and
``usb-device-video`` with a firmware that streams; the runner copies the
scripts it runs. The runner never touches a board on the port (Espressif's
VID) unless it is the peer.

A pass means every check ran. Give neither ``--peer`` nor ``--host-only`` and
the runner stops before it touches a board. ``--host-only`` is the explicit
way to run enumerate, msc and hid alone: the five checks that need a peer are
named as skipped, and the verdict says so, ``HOST LEG PASS 3 of 3 (host-only;
5 need --peer)``. Any other skip, such as hid finding no keyboard on the port,
fails the run: ``HOST LEG FAIL 2 of 3 (host-only; 5 need --peer; unexpected
skip: hid)``.

``--plant all`` (or a comma list) turns on each check's planted fault: the
drive missing from the list, a byte of the file corrupted on the way, the
keyboard left in report protocol, a typed report lost, the wrong sum typed,
every 50th note dropped, the tone stopped halfway, the last frame's JPEG
header lost. Each planted check must fail; the run ends ``HOST LEG FAIL``
and ``PLANTS CAUGHT n of n``.

Prints one JSON line per check and ends with ``HOST LEG PASS`` or
``HOST LEG FAIL``. Exit 0 on pass, 1 on fail, 2 on a bad command line; in a planted run, exit 0 only
if every plant was caught.
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS_HOST = os.path.join(HERE, "..", "host")
EXAMPLES = os.path.join(HERE, "..", "..", "examples")
MPFTP = os.path.expanduser("~/gh/pydevices/mpftp/scripts/mpftp")
CHECKS = ("enumerate", "msc", "hid", "hid_typed", "cdc", "midi", "audio", "video")
NEEDS_PEER = {"hid_typed", "cdc", "midi", "audio", "video"}
WHAT_PEER = {
    "hid_typed": "a peer board typing through usb_hid",
    "cdc": "a peer board's REPL on its USB serial port",
    "midi": "a peer board running the MIDI echo",
    "audio": "a peer board presenting usb-device-audio",
    "video": "a peer board presenting usb-device-video",
}
CLASS_CODES = {"msc": 0x08, "hid": 0x03, "cdc": 0x02, "midi": 0x01, "audio": 0x01, "video": 0x0E}
HOST_FILES = [os.path.join(HERE, "host_board.py")] + [
    os.path.join(TOOLS_HOST, f) for f in ("msc_mount_check.py", "kbd_host.py", "cdc_check.py",
                                          "audio_check.py", "video_check.py")]
PEER_FILES = [os.path.join(TOOLS_HOST, "kbd_device.py"), os.path.join(HERE, "matrix_board.py"),
              os.path.join(EXAMPLES, "usb_soundcard.py"), os.path.join(EXAMPLES, "usb_webcam.py")]


def sh(cmd, timeout=120):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        return 124, "timed out after %ss: %s" % (timeout, e.output or "")
    return r.returncode, r.stdout + r.stderr


class Board:
    """One held REPL, always named by its device: another agent's holder may
    be running beside this one, and an unnamed hold command would find it."""

    def __init__(self, dev):
        self.dev = dev

    def hold(self, *args, timeout=60):
        return sh([MPFTP, "hold", *args, "-d", self.dev], timeout=timeout)

    def run(self, code, timeout=30):
        return self.hold("ask", code, "--timeout", str(timeout), timeout=timeout + 30)

    def value(self, code, timeout=60):
        """What ``print('@@', code)`` printed, or ERROR and the tail."""
        rc, out = self.hold("ask", "print('@@', %s)" % code, "--marker", "@@", "--text",
                            "--timeout", str(timeout), timeout=timeout + 30)
        return out.strip() if rc == 0 else "ERROR " + out[-300:]

    def start(self, modules):
        self.hold("stop", timeout=30)
        rc, out = self.hold("start", timeout=60)
        if rc:
            sys.exit("could not hold %s: %s" % (self.dev, out[-300:]))
        pops = "; ".join("sys.modules.pop(%r, None)" % m for m in modules)
        for _ in range(10):
            # Opening a UART bridge can reset the board, and a board that
            # boots into an app isn't at the prompt: interrupt until it answers.
            self.hold("interrupt", timeout=30)
            self.run("import sys; %s" % pops, timeout=20)
            if self.value("1 + 1", timeout=10) == "2":
                return
            time.sleep(2)
        sys.exit("%s never answered at the prompt" % self.dev)

    def stop(self):
        self.hold("stop", timeout=30)


def put(dev, files):
    for f in files:
        rc, out = sh([MPFTP, "put", "-d", dev, f, "/" + os.path.basename(f)], timeout=120)
        if rc:
            sys.exit("could not copy %s to %s: %s" % (os.path.basename(f), dev, out[-300:]))


def board_check(host, call, timeout):
    out = host.value("host_board.%s" % call, timeout=timeout)
    try:
        return json.loads(out)
    except ValueError:
        return {"error": out[-300:]}


def judge_enumerate(r, expect):
    sys.path.insert(0, HERE)
    from desc_check import faults

    if "error" in r:
        return False, {"error": r["error"]}
    devices, bad, unnamed, present = [], {}, [], set()
    for d in r["devices"]:
        devices.append("%s %s/%s" % (d["id"], d["names"][0], d["names"][1]))
        if d["names"][0] == "!":
            unnamed.append(d["id"])
        speed = {1: "low", 2: "full", 3: "high"}.get(d["speed"], "full")
        # A third-party device answers to its own controller: no IN limit here.
        f = faults(bytes.fromhex(d["config"]), speed, 15)
        if f:
            bad[d["id"]] = f
        present.update(d["classes"])
    missing = [c for c in expect if CLASS_CODES[c] not in present]
    row = {"devices": devices, "enum_errors": r["errors"]}
    if missing:
        row["missing"] = missing
    if bad:
        row["desc_faults"] = bad
    if unnamed:
        row["unnamed"] = unnamed
    return not missing and not bad and not unnamed and r["errors"] == 0, row


def judge(name, r):
    if "error" in r:
        return False, r
    if name == "msc":
        return bool(r["same"] and r["removed"] and r["listing"] is not None), r
    if name == "hid":
        return bool(r["boot_protocol"] and r["report_descriptor"] > 0 and r["quiet_reads"] == r["reads"]
                    and r["ms"] < r["reads"] * 300), r
    if name == "hid_typed":
        # a, release, b, release, shift+C, release, in boot protocol.
        want = ["0000040000000000", "0000000000000000", "0000050000000000", "0000000000000000",
                "0200060000000000", "0000000000000000"]
        r["expected"] = len(want)
        return r["reports"] == want, r
    if name == "cdc":
        return bool(r["answered"]), r
    if name == "midi":
        return r["received"] == r["sent"] and r["in_order"], r
    if name == "audio":
        st = r.get("stats") or ()
        r_ok = r["sent"] >= 0.9 * r["expected"] and (len(st) < 5 or st[4] == 0)
        return bool(r_ok), r
    if name == "video":
        mode = r["mode"]
        decoded_ok = mode[0] != "mjpeg" or r.get("decoded") == [mode[1], mode[2]]
        return bool(r["frames"] and r["fps"] >= 5 and decoded_ok), r
    return False, r


def with_peer(peer, name, host, call, timeout, plant):
    """Bring the peer's device side up for this check, run the host's half,
    and put the peer back to its own serial port."""
    restore = "import usb.device; usb.device.get().init(builtin_driver=True)"
    if name == "cdc":
        return board_check(host, call, timeout)  # the peer's REPL is its own serial port
    if name == "hid_typed":
        peer.run("import kbd_device; kbd_device.start()", timeout=20)
        t = threading.Thread(target=lambda: peer.run("time.sleep(3); kbd_device.type_keys()", timeout=30))
        peer.run("import time", timeout=10)
        t.start()
        r = board_check(host, call, timeout)
        t.join()
        peer.run(restore, timeout=20)
        return r
    if name == "midi":
        peer.run("import matrix_board; matrix_board.setup(('midi',))", timeout=30)
        if plant:
            peer.run("matrix_board._drop = 0", timeout=10)
        t = threading.Thread(target=lambda: peer.run("matrix_board.serve(40)", timeout=60))
        t.start()
        time.sleep(3)
        r = board_check(host, call, timeout)
        t.join()
        peer.run("matrix_board._drop = None; matrix_board.stop()", timeout=20)
        return r
    # audio and video: the example runs in the peer's foreground until interrupted.
    script = {"audio": "import usb_soundcard", "video": "import usb_webcam; usb_webcam.main()"}[name]
    peer.hold("send", script, timeout=30)
    time.sleep(8)
    r = board_check(host, call, timeout)
    peer.hold("interrupt", timeout=30)
    peer.run(restore, timeout=20)
    return r


def verdict(results, host_only):
    """The last line and the exit status for a list of check rows.

    A pass is every selected check run and passed. A row with ``skipped`` is
    expected only when ``--host-only`` was given and the check needs a peer;
    any other skip is a failure."""
    ran = [r for r in results if "pass" in r]
    expected = [r["check"] for r in results if r.get("skipped") and host_only and r["check"] in NEEDS_PEER]
    unexpected = [r["check"] for r in results if r.get("skipped") and r["check"] not in expected]
    good = bool(ran) and all(r["pass"] for r in ran) and not unexpected
    notes = []
    if host_only:
        notes.append("host-only")
    if expected:
        notes.append("%d need --peer" % len(expected))
    if unexpected:
        notes.append("unexpected skip: " + ", ".join(unexpected))
    tail = " (%s)" % "; ".join(notes) if notes else ""
    line = "HOST LEG %s %d of %d%s" % ("PASS" if good else "FAIL", sum(r["pass"] for r in ran), len(ran) + len(unexpected), tail)
    return good, line


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", required=True, help="the host board's console (a UART, or ws://HOST)")
    ap.add_argument("--peer", help="a peer board's console; its device port is on the host's port")
    ap.add_argument("--host-only", action="store_true",
                    help="no peer: run only the checks that need none, and say so in the verdict")
    ap.add_argument("--expect", default="msc,hid", help="classes that must be on the port (default msc,hid)")
    ap.add_argument("--only", help="comma list of checks to run")
    ap.add_argument("--plant", help="'all' or a comma list of checks whose planted fault is on")
    ap.add_argument("--no-put", action="store_true", help="the board files are already in place")
    args = ap.parse_args()
    if args.peer and args.host_only:
        ap.error("--peer and --host-only contradict each other")
    if not args.peer and not args.host_only:
        ap.error("name a --peer, or say --host-only to run just the checks that need none "
                 "(a pass means every check ran)")

    only = args.only.split(",") if args.only else CHECKS
    plants = set(CHECKS if args.plant == "all" else (args.plant.split(",") if args.plant else ()))
    expect = [c for c in args.expect.split(",") if c]
    if not args.no_put:
        put(args.host, HOST_FILES)
        if args.peer:
            put(args.peer, PEER_FILES)
    host = Board(args.host)
    host.start(["host_board", "msc_mount_check", "kbd_host", "cdc_check", "audio_check", "video_check"])
    host.run("import host_board", timeout=30)
    peer = None
    if args.peer:
        peer = Board(args.peer)
        peer.start(["kbd_device", "matrix_board"])

    results = []
    for name in CHECKS:
        if name not in only:
            continue
        row = {"check": name}
        if name in NEEDS_PEER and peer is None:
            row["skipped"] = "needs %s (--peer)" % WHAT_PEER[name]
            results.append(row)
            print(json.dumps(row), flush=True)
            continue
        planted = name in plants
        host.run("host_board.PLANT = %r" % ({name} if planted else set()), timeout=10)
        if planted:
            row["planted"] = True
        call = {"enumerate": "check_enumerate()", "msc": "check_msc()", "hid": "check_hid()",
                "hid_typed": "check_hid_typed(8)", "cdc": "check_cdc()", "midi": "check_midi(200)",
                "audio": "check_audio(3)", "video": "check_video(3)"}[name]
        timeout = {"enumerate": 40, "midi": 60, "audio": 40, "video": 40}.get(name, 30)
        if name in NEEDS_PEER:
            r = with_peer(peer, name, host, call, timeout, planted)
        else:
            r = board_check(host, call, timeout)
        if r.get("skipped"):
            row["skipped"] = r["skipped"]
            results.append(row)
            print(json.dumps(row), flush=True)
            continue
        ok, detail = judge_enumerate(r, expect) if name == "enumerate" else judge(name, r)
        row.update(detail)
        row["pass"] = ok
        results.append(row)
        print(json.dumps(row), flush=True)

    host.run("host_board.PLANT = set(); import usb.core; usb.core.stop()", timeout=20)
    host.stop()
    if peer:
        peer.stop()
    ran = [r for r in results if "pass" in r]
    good, line = verdict(results, args.host_only)
    print(line)
    if plants:
        tried = [r for r in ran if r.get("planted")]
        caught = [r for r in tried if not r["pass"]]
        print("PLANTS CAUGHT %d of %d" % (len(caught), len(tried)))
        sys.exit(0 if tried and len(caught) == len(tried) else 1)
    sys.exit(0 if good else 1)


if __name__ == "__main__":
    main()
