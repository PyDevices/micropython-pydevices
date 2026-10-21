# Firmware sets

A firmware set is every board's flashable file, built from one commit of this
repository by the **Release firmware** workflow
([`release-firmware.yml`](../.github/workflows/release-firmware.yml); the cells
are in [`scripts/release_firmware.py`](../scripts/release_firmware.py)).
Each cell is one `build_mp.py` call; the `assemble` job checks every file
(checksums, the ESP image magic, the app image inside the flash image, UF2
blocks) and writes `firmware.json`, which records every file's sha256 and size,
the flash offset, and the pins the set was built from.

## Build a set

Actions > Release firmware > Run workflow. A pull request that touches the
recipe, and a run every Monday, build the same cells and keep the files as a
run artifact for 14 days. They publish nothing.

## Put a set on a draft Release

Tick **draft_release** and give a **tag**. After `assemble`, the `draft` job
attests every file (`gh attestation verify <file> --repo PyDevices/micropython-pydevices`)
and creates a *draft* GitHub Release holding the flash images, the `.elf` and
`.map` of each build, and `firmware.json`. A draft is seen only by people who
can push to the repository, and GitHub makes no tag until someone publishes it.

The tag is `v<MicroPython version>-<YYYYMMDD>`. A set built from a preview
MicroPython or CircuitPython only takes a tag that starts with `staged-`, so a
preview set can't be published by accident.

## The download page

[`scripts/stage_firmware_page.py`](../scripts/stage_firmware_page.py) lays a
set out for the site's `firmware/` folder: `firmware.json`, the flash images
the page offers and the browser flasher fetches, an ESP Web Tools manifest per
ESP32 build, and the desktop interpreters' `interpreters.json` from the newest
pydevices final that has one.

```
python3 scripts/stage_firmware_page.py <downloaded firmware artifact> <site checkout>/firmware \
    --interpreters-from-release PyDevices/pydevices
```
