# Newcomer's guide to micropython-pydevices

`micropython-pydevices` is PyDevices' versioned MicroPython runtime overlay. It holds the patch queue, user-module manifests, board directories, and build variants applied to one pinned upstream MicroPython release. It is not a fork and it does not publish a Python package.

## Start by selecting a preset

Choose the target shape before modifying a MicroPython checkout:

- A `profile` selects an ordered subset of upstream patches.
- A `manifest` selects the sibling PyDevices user modules frozen into a build.
- A `board` supplies ESP32 board-specific sdkconfig, partitions, and defaults.
- A `variant` supplies Unix, Windows, or WebAssembly build configuration.

The root [README](../README.md) is the source of truth for these categories. `apply.sh <profile> <micropython-dir> --check` verifies whether a checkout matches a named patch profile before modifying it.

## The mental model

```text
pinned upstream MicroPython tag
             |
             v
ordered profile patch series
             |
             v
clean prepared checkout
             |
     +-------+--------+
     |                |
manifest         board or variant
     |                |
     +-------+--------+
             v
       upstream build command
```

`tools/prepare-micropython.sh` prepares a sibling checkout at the pinned tag and records the selected overlay work locally. After preparation, builds use upstream's normal tools; PyDevices-specific choices are expressed by the profile, manifest, board, and variant paths.

## Repository map

| Path | Purpose |
|---|---|
| `UPSTREAM` | Exact upstream MicroPython release the overlay applies to. |
| `patches/` | Ordered mailbox patches with individual provenance. |
| `profiles/` | Named ordered subsets of patch numbers. |
| `manifests/` | Frozen-module presets that include sibling repositories. |
| `boards/esp32/` | Out-of-tree ESP32 boards, sdkconfig, partitions, and defaults. |
| `variants/` | Out-of-tree Unix, Windows, and WebAssembly variants. |
| `tools/prepare-micropython.sh` | Pinned-checkout preparation tool. |
| `apply.sh` | Apply or verify a profile against a checkout. |
| `provenance.json` | Patch checksums and migration records. |

## Important boundaries

The upstream tag is a compatibility boundary. Moving `UPSTREAM` requires revalidating every patch and changing the overlay release identity. Do not hand-edit an already patched MicroPython checkout; change the overlay source and regenerate the affected patch.

Profiles are deliberately different. The `vst3-engine` profile excludes networking and FFI patches so untrusted plugin content cannot acquire those capabilities. Do not replace it with a broader desktop profile merely because it builds.

A manifest names modules from sibling repositories. It controls what freezes into firmware, not what gets installed later with MIP. Consult [the manifests guide](../manifests/README.md) before adding a repository or duplicating a preset.

## Safe first contributions

Start with documentation, provenance, or a narrowly scoped profile/manifest correction. Validate a profile with `apply.sh --check` against a clean checkout before changing patches. Board and variant work should preserve the split between upstream configuration and PyDevices-owned overlay files.
