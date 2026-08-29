# Profiles

A profile is an ordered subset of `../patches` (plus, where noted, the
usermods/variants this repo carries). `apply.sh <profile> <micropython-dir>`
applies one; `--check` verifies applicability without touching the tree.

| Profile | Patches | Extras |
|---|---|---|
| `windows-networked` | 0001 | |
| `windows-full` | 0001, 0002, 0003 | |
| `desktop-pydevices` | 0002 | |
| `webassembly-pydevices` | 0004, 0005, 0006, 0007, 0008 | `usermods/wasmbridge`, `variants/webassembly` |
