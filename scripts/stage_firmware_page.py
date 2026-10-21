#!/usr/bin/env python3
"""Lay a firmware set out for the download page.

    stage_firmware_page.py <firmware-dir> <page-dir> [--interpreters FILE |
                           --interpreters-from-release OWNER/REPO]

<firmware-dir> is what the Release firmware workflow's `assemble` job left
(firmware.json and the files it names). <page-dir> is the `firmware/` folder of
the site. The page reads three things from it, and this writes all three,
replacing whatever the last set left:

    firmware.json       the set's description, as the workflow wrote it
    files/              the flash image (or UF2) of every build, which is what
                        the page offers and what the browser flasher fetches
                        (a release download carries no CORS headers, so the
                        flasher cannot read it from the Release)
    manifests/<id>.json an ESP Web Tools manifest for each ESP32 build
    interpreters.json   the desktop interpreters of the newest pydevices final,
                        when one is given or found (see below)

Every file is checked against the sha256 in firmware.json on the way in. The
ELF, map, app image and partition table stay on the Release; the page links
them there.

--interpreters-from-release asks GitHub for the newest final release of that
repository that carries an `interpreters.json` asset and copies it. Releases
before the interpreters workflow have none, and the page then says the desktop
interpreters arrive with the next final.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

CHIP_FAMILY = {"esp32s3": "ESP32-S3", "esp32p4": "ESP32-P4", "esp32": "ESP32", "esp32c3": "ESP32-C3",
               "esp32c6": "ESP32-C6", "esp32s2": "ESP32-S2"}
SERVED = ("flash", "uf2")


class Failed(Exception):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def manifest(doc, build):
    """ESP Web Tools' manifest for one build: one part, the merged image, at its flash offset."""
    flash = next(f for f in build["files"] if f["role"] == "flash")
    interpreter = "MicroPython" if build["interpreter"] == "micropython" else "CircuitPython-compatible"
    return {
        "name": f"OmniPython {interpreter} for the {build['label']}",
        "version": doc["tag"],
        # Asks "erase the device first?" and says all data is lost.
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [{"chipFamily": CHIP_FAMILY[build["chip"]],
                    "parts": [{"path": f"../files/{flash['name']}", "offset": int(build["flash_offset"], 16)}]}],
    }


def newest_final_with_interpreters(releases):
    """The newest published, non-pre-release release carrying interpreters.json (GitHub lists newest first)."""
    for r in releases:
        if r.get("draft") or r.get("prerelease"):
            continue
        if any(a["name"] == "interpreters.json" for a in r.get("assets", [])):
            return r["tag_name"]
    return None


def fetch_interpreters(repo):
    out = subprocess.run(["gh", "api", f"repos/{repo}/releases?per_page=100", "--jq", "."],
                         capture_output=True, text=True)
    if out.returncode:
        raise Failed(f"gh could not list {repo}'s releases: {out.stderr.strip()}")
    tag = newest_final_with_interpreters(json.loads(out.stdout))
    if not tag:
        return None
    with tempfile.TemporaryDirectory() as d:
        r = subprocess.run(["gh", "release", "download", tag, "-R", repo, "-p", "interpreters.json", "-D", d],
                           capture_output=True, text=True)
        if r.returncode:
            raise Failed(f"gh could not download {tag}'s interpreters.json: {r.stderr.strip()}")
        return json.loads((Path(d) / "interpreters.json").read_text())


def stage(firmware_dir, page_dir, interpreters=None):
    src, dst = Path(firmware_dir), Path(page_dir)
    doc = json.loads((src / "firmware.json").read_text())
    todo = []
    for b in doc["builds"]:
        for f in b["files"]:
            if f["role"] in SERVED:
                p = src / f["name"]
                if not p.is_file():
                    raise Failed(f"{f['name']} is not in {src}")
                if sha256(p) != f["sha256"]:
                    raise Failed(f"{f['name']} does not match the sha256 in firmware.json")
                todo.append((p, f["name"]))
    if interpreters is not None and not (isinstance(interpreters, dict) and "tag" in interpreters
                                         and isinstance(interpreters.get("files"), dict)):
        raise Failed("interpreters.json needs a tag and a files table")

    for sub in ("files", "manifests"):
        shutil.rmtree(dst / sub, ignore_errors=True)
        (dst / sub).mkdir(parents=True)
    for p, name in todo:
        shutil.copyfile(p, dst / "files" / name)
    for b in doc["builds"]:
        if b["chip"] in CHIP_FAMILY and b["flash_offset"] is not None:
            (dst / "manifests" / f"{b['id']}.json").write_text(json.dumps(manifest(doc, b), indent=1) + "\n")
    shutil.copyfile(src / "firmware.json", dst / "firmware.json")
    (dst / "interpreters.json").unlink(missing_ok=True)
    if interpreters is not None:
        (dst / "interpreters.json").write_text(json.dumps(interpreters, indent=1) + "\n")
    return doc


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("firmware_dir")
    ap.add_argument("page_dir")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--interpreters")
    g.add_argument("--interpreters-from-release", metavar="OWNER/REPO")
    args = ap.parse_args(argv)
    try:
        interp = None
        if args.interpreters:
            interp = json.loads(Path(args.interpreters).read_text())
        elif args.interpreters_from_release:
            interp = fetch_interpreters(args.interpreters_from_release)
            if interp is None:
                print(f"note: no final release of {args.interpreters_from_release} carries interpreters.json")
        doc = stage(args.firmware_dir, args.page_dir, interp)
    except (Failed, OSError, ValueError, KeyError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    size = sum(f.stat().st_size for f in (Path(args.page_dir) / "files").iterdir())
    print(f"staged {doc['tag']}: {len(doc['builds'])} builds, {size / 1048576:.1f} MB in files/"
          + (f", interpreters from {interp['tag']}" if interp else ", no interpreters.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
