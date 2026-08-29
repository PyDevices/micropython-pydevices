# micropython-pydevices

The versioned runtime overlay for MicroPython in the PyDevices project:
every downstream patch, usermod, and variant PyDevices maintains on top of
a **pinned upstream release** (`UPSTREAM`, currently v1.28.0), kept the way
a distribution keeps its patch queue — an ordered mailbox series with
provenance, applied to a clean tree, never a fork.

## Layout

- `UPSTREAM` — the upstream MicroPython release this series applies to.
- `patches/` — the ordered mailbox series (`0001-…` to `0008-…`): Windows
  networking/sockets/select/SSL, Windows FFI, desktop scheduler depth, and
  the WebAssembly set (Asyncify, node hooks, soft reinitialization, jsffi
  across reinit, lexer EOF).
- `profiles/` — named subsets: `windows-networked`, `windows-full`,
  `desktop-pydevices`, `webassembly-pydevices` (`*.series` = ordered patch
  numbers).
- `usermods/wasmbridge/` — the wasm bridge user C module.
- `variants/webassembly/` — the external WebAssembly variant tree
  (including the Fetch-backed `requests`).
- `provenance.json` — patch checksums and migration provenance.
- `apply.sh <profile> <micropython-dir> [--check]` — apply or verify a
  profile against a checkout.

## Rules

- Upstream is **pinned**; moving `UPSTREAM` re-validates every patch and
  bumps the overlay release id (`mp-v1.28.0-pydevices.N`).
- Patches are individually justified in their headers; no hand-edits to a
  patched tree — regenerate the patch.
- Publishing overlay *binaries* is a separate decision from this source
  repository.

Migrated from `PyDevices/cmods` (2026-08-29), which now consumes this
overlay rather than owning it.
