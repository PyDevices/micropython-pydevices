"""The device-class proof matrix: every combination enumerates on Windows and
moves data. Runs in WSL; drives the board over WebREPL with ``mpftp hold``.

    python3 tools/matrix/run_matrix.py --ws 192.168.1.159 --serial DCDA0C48AC0C [--only msc,hid]
    python3 tools/matrix/run_matrix.py --dev COM4 --serial E8F60AE0F070 --in-max 7   # the P4 panel

The board needs ``matrix_board.py`` at its root, micropython-lib's
usb-device-cdc, -hid and -midi, ``usb_hid``, ``usb-device-msc``, ``midiports``
and ``adafruit_hid``, and WebREPL. Each combination is brought up beside the
board's own serial port with ``matrix_board.setup()``, then:

- enumeration: Windows lists a function for each class, under the board's
  USB serial number, at the speed the port runs, and every bulk endpoint it
  received is 512 bytes at high speed or 64 at full (``usb_desc.py``);
- msc: Windows reads the board's file and writes one the board reads back;
- hid: 100 mouse reports counted through Raw Input (``rawinput_count.py``);
- midi: 200 note-ons round-trip through the board's echo, in order
  (``midi_roundtrip.py``, paced);
- cdc: 1,000 bytes echoed through the extra serial port (``cdc_echo.py``).

A combination the controller can't serve (the S3 has IN endpoints up to 4)
must be refused by ``USBDevice.config()`` with a ValueError, and that counts
as its expected result. Prints one JSON line per combination and ends with
``MATRIX PASS`` or ``MATRIX FAIL``.

``--plant`` breaks one check on purpose (the MIDI echo drops every 50th note)
so you can see the matrix fail.
"""
import argparse
import itertools
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MPFTP = os.path.expanduser("~/gh/pydevices/mpftp/scripts/mpftp")
WIN_TMP = "/mnt/c/Users/bradb/AppData/Local/Temp/usbif-matrix"
CLASSES = ("cdc", "hid", "midi", "msc")
# IN endpoints each function takes; the board's own serial port takes two.
IN_EPS = {"cdc": 2, "hid": 1, "midi": 1, "msc": 1}


def sh(cmd, timeout=120, cwd=None):
    try:
        r = subprocess.run(cmd, shell=isinstance(cmd, str), capture_output=True, text=True, timeout=timeout, cwd=cwd)
    except subprocess.TimeoutExpired as e:
        return 124, "timed out after %ss: %s" % (timeout, e.output or "")
    return r.returncode, r.stdout + r.stderr


def win_py(script, *args, timeout=120):
    return sh(["python.exe", script, *args], timeout=timeout, cwd=WIN_TMP)


def ask(code, timeout=60):
    """Run code in the held REPL; returns what it printed after the @@ marker."""
    rc, out = sh([MPFTP, "hold", "ask", "print('@@', repr(%s))" % code, "--marker", "@@", "--text",
                  "--timeout", str(timeout)], timeout=timeout + 30)
    # With --marker and --text the holder prints just what followed the marker.
    return out.strip() if rc == 0 else "ERROR " + out[-300:]


def functions_on_windows(serial):
    ps = ("Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like '*VID_303A*' -and "
          "$_.InstanceId -like '*MI_*' } | ForEach-Object { $_.Class + '|' + $_.FriendlyName + '|' + $_.InstanceId }")
    rc, out = sh(["powershell.exe", "-NoProfile", "-Command", ps], timeout=60)
    # Interface children carry the parent's serial in their parent chain, not
    # their own id, so match by the composite's container: list the composite's
    # children instead.
    ps2 = ("$p = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like '*VID_303A*%s*' }; "
           "foreach ($d in $p) { (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Children)"
           ".Data }" % serial)
    rc, kids = sh(["powershell.exe", "-NoProfile", "-Command", ps2], timeout=60)
    kids = {k.strip().upper() for k in kids.splitlines() if k.strip()}
    found = []
    for line in out.splitlines():
        parts = line.strip().split("|")
        if len(parts) == 3 and parts[2].upper() in kids:
            found.append((parts[0], parts[1]))
    return found


def check_enumeration(names, found):
    classes = {c for c, _ in found}
    need = {"cdc": "Ports", "hid": "HIDClass", "midi": "MEDIA", "msc": "USB"}
    missing = []
    serial_ports = sum(1 for c, _ in found if c == "Ports")
    if "cdc" in names and serial_ports < 2:
        missing.append("cdc")
    for n in names:
        if n != "cdc" and need[n] not in classes:
            missing.append(n)
    return missing


def check_msc():
    # Windows mounts the volume a few seconds after the drive enumerates.
    ps = ("$v = Get-Volume | Where-Object { $_.DriveType -eq 'Removable' -and $_.Size -gt 0 -and $_.Size -lt 200000 }; "
          "foreach ($x in $v) { $d = $x.DriveLetter; if (Test-Path \"${d}:\\hello.txt\") { "
          "$t = Get-Content \"${d}:\\hello.txt\"; Set-Content -Path \"${d}:\\from_pc.txt\" -Value 'pc'; "
          "Write-VolumeCache -DriveLetter $d; $t; break } }")
    read_ok = False
    for _ in range(10):
        rc, out = sh(["powershell.exe", "-NoProfile", "-Command", ps], timeout=60)
        if "matrix" in out:
            read_ok = True
            break
        time.sleep(1.5)
    back = ask("matrix_board.msc_read('from_pc.txt')") if read_ok else ""
    return {"msc_pc_read": read_ok, "msc_board_read": "pc" in back, "pass": read_ok and "pc" in back}


def check_hid():
    proc = subprocess.Popen(["python.exe", "rawinput_count.py", "VID_303A", "12"], cwd=WIN_TMP,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(2)
    ask("matrix_board.hid_burst(100)", timeout=40)
    out = proc.communicate(timeout=60)[0]
    try:
        counts = json.loads(out.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return {"pass": False, "hid_error": out[-200:]}
    return {"hid_mouse": counts["mouse"], "hid_net_dx": counts["dx"], "pass": counts["mouse"] == 100 and counts["dx"] == 0}


def _with_echo(cmd, seconds):
    """Run a Windows-side check while the board echoes in the foreground.
    The board's loop starts first: a hold ask takes a second or two over
    WebREPL, and anything sent before the loop runs comes back late."""
    import threading

    t = threading.Thread(target=ask, args=("matrix_board.serve(%d)" % seconds,),
                         kwargs={"timeout": seconds + 20})
    t.start()
    time.sleep(3)
    rc, out = win_py(*cmd, timeout=seconds + 20)
    t.join()
    return out


_MIDI_LIST = ("import sys; sys.path.insert(0, '.'); from midiports import windows; "
              "print('|'.join(sorted({p.name for p in windows.available()})))")


def midi_names():
    rc, out = win_py("-c", _MIDI_LIST, timeout=30)
    lines = [l for l in out.splitlines() if "|" in l or l.strip()]
    return set(lines[-1].strip().split("|")) if lines else set()


def check_midi(before):
    # The board's port is the one that appeared after this combination's
    # setup (another board may present a port with the same name); Windows may
    # still be loading the MIDI driver just after enumeration.
    name = None
    for _ in range(15):
        new = midi_names() - before
        if new:
            name = sorted(new)[0]
            break
        time.sleep(1)
    if name is None:
        return {"pass": False, "midi_error": "no new MIDI port appeared"}
    out = _with_echo(["midi_roundtrip.py", name, "--notes", "200", "--mode", "paced"], 25)
    try:
        r = json.loads([l for l in out.splitlines() if l.startswith("{")][-1])
    except (ValueError, IndexError):
        return {"pass": False, "midi_error": out[-200:]}
    return {"midi_port": name, "midi_received": r["received"], "midi_in_order": r["in_order"], "pass": r["pass"]}


def check_cdc(serial):
    out = _with_echo(["cdc_echo.py", serial, "1000"], 10)
    try:
        r = json.loads(out.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return {"pass": False, "cdc_error": out[-200:]}
    return {"cdc_received": r.get("received"), "pass": r["pass"]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ws", help="the board's WebREPL address")
    ap.add_argument("--dev", help="or a serial console that isn't the USB port under test (a UART bridge)")
    ap.add_argument("--serial", required=True, help="the board's USB serial number (hex)")
    ap.add_argument("--only", help="comma list: test only combinations of these classes")
    ap.add_argument("--in-max", type=int, default=4, help="the controller's highest IN endpoint")
    ap.add_argument("--plant", action="store_true")
    args = ap.parse_args()

    os.makedirs(WIN_TMP, exist_ok=True)
    for f in ("rawinput_count.py", "cdc_echo.py", "usb_desc.py"):
        sh(["cp", os.path.join(HERE, f), WIN_TMP])
    sh(["cp", os.path.expanduser("~/gh/pydevices/.toolbox/scripts/midi_roundtrip.py"), WIN_TMP])
    lib = os.path.join(WIN_TMP, "lib")
    if not os.path.isdir(os.path.join(WIN_TMP, "midiports")):
        sh(["cp", "-r", os.path.expanduser("~/gh/pydevices/pydevices/midiports/midiports"), WIN_TMP])
        sh(["cp", os.path.expanduser("~/gh/pydevices/pydevices/utils/uwin32.py"), WIN_TMP])
        sh(["cp", "-r", "/tmp/midiports-adafruit_midi/1.6.0/adafruit_midi", WIN_TMP])

    sh([MPFTP, "hold", "stop"], timeout=30)
    if not (args.ws or args.dev):
        ap.error("--ws or --dev is needed")
    rc, out = sh([MPFTP, "hold", "start", "-d", args.dev or "ws://" + args.ws], timeout=60)
    # The board's REPL outlives a run: drop any matrix_board an earlier run
    # imported, so this run uses the file on the board now.
    # Opening a UART bridge can reset the board: retry until it answers.
    for _ in range(10):
        sh([MPFTP, "hold", "ask", "import sys; sys.modules.pop('matrix_board', None); import matrix_board",
            "--timeout", "20"], timeout=60)
        if ask("'matrix_board' in sys.modules") == "True":
            break
        time.sleep(2)
    else:
        sys.exit("the board never imported matrix_board")
    if args.plant:
        sh([MPFTP, "hold", "ask", "matrix_board._drop = 0", "--timeout", "10"], timeout=40)

    classes = args.only.split(",") if args.only else CLASSES
    results = []
    for k in range(1, len(classes) + 1):
        for combo in itertools.combinations(classes, k):
            need = 2 + sum(IN_EPS[c] for c in combo)
            ask("matrix_board.stop()", timeout=30)
            time.sleep(3)
            before = midi_names() if "midi" in combo else set()
            refused = ask("matrix_board.setup(%r)" % (combo,), timeout=60)
            row = {"combo": "+".join(combo), "in_endpoints": need}
            if need > args.in_max:
                row["refused"] = refused
                row["pass"] = "IN endpoint" in refused
                results.append(row)
                print(json.dumps(row), flush=True)
                continue
            if refused != "None":
                row.update({"pass": False, "setup": refused})
                results.append(row)
                print(json.dumps(row), flush=True)
                continue
            time.sleep(6)
            rc, out = win_py("usb_desc.py", "303A", args.serial, timeout=60)
            try:
                d = json.loads(out.strip().splitlines()[-1])
                bulk = sorted({e["max_packet"] for e in d["endpoints"] if e["type"] == "bulk"})
                row["speed"] = d["speed"]
                row["bulk_sizes"] = bulk
                # USB 2.0: bulk endpoints are 512 bytes at high speed, 64 at full.
                row["bulk_ok"] = bulk == [512 if d["speed"] == "high" else 64]
                sys.path.insert(0, HERE)
                from desc_check import faults

                bad = faults(bytes.fromhex(d["config"]), d["speed"], args.in_max)
                if bad:
                    row["desc_faults"] = bad
                    row["bulk_ok"] = False
            except (ValueError, IndexError, KeyError):
                row["bulk_ok"] = False
            found = functions_on_windows(args.serial)
            missing = check_enumeration(combo, found)
            row["enumerated"] = not missing
            if missing:
                row["missing"] = missing
            ok = not missing and row["bulk_ok"]
            for c in combo:
                if c == "midi":
                    r = check_midi(before)
                elif c == "cdc":
                    r = check_cdc(args.serial)
                else:
                    r = {"msc": check_msc, "hid": check_hid}[c]()
                ok = ok and r.pop("pass")
                row.update(r)
            row["pass"] = ok
            results.append(row)
            print(json.dumps(row), flush=True)
    sh([MPFTP, "hold", "ask", "matrix_board.stop()", "--timeout", "10"], timeout=40)
    sh([MPFTP, "hold", "stop"], timeout=30)
    good = all(r["pass"] for r in results)
    print("MATRIX", "PASS" if good else "FAIL", "%d of %d" % (sum(r["pass"] for r in results), len(results)))
    sys.exit(0 if good else 1)


if __name__ == "__main__":
    main()
