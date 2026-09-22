# Presets

Each file here is a frozen manifest a build is pointed at with
`FROZEN_MANIFEST=`: a short list of `include()` lines naming the sibling
repositories the build carries, after a prologue that pulls in upstream's
own frozen content for the port. Every repository with a C half names it
from its own `manifest.py` (MicroPython 1.29's `c_module()`), so a preset is
only a list.

| Preset | Carries |
|---|---|
| `headless.py` | upstream's own content only -- no display stack, no audio |
| `lvgl.py` | displayif, lvgl-micropython |
| `pygraphics.py` | pygraphics, displayif, pdwidgets, palettes |
| `audio.py` | audiodsp, audioif |
| `usb.py` | usbif |
| `kitchen-sink.py` | every sibling that has a manifest -- what the workspace built before presets existed |
| `vst3-engine.py` | the kitchen sink plus the engine's two usermods |

Paths are relative to this directory, so the layout is the one
`tools/make-work-dir.sh` lays out: this repository and the module
repositories as siblings of the MicroPython clone. A user who wants a set
nobody wrote makes a file of the same shape.

ulab is upstream's repository with no manifest of ours: on the CMake board
ports a preset names `../../ulab/code`; on the Make ports audiodsp's own
`micropython.mk` includes it, so naming it twice would compile it twice.
The webassembly variant has its own manifest with a copy of the kitchen
sink's scan, because its `requests` module must be frozen before anything
resolves the socket-backed one that a preset's prologue would pull in.

pydevices itself is never frozen: every host installs it with mip, so what
runs is what is published or staged, never a build-time snapshot that
shadows it. Its `manifest.py` packages the tree for mip, which is why the
kitchen sink skips it.

On the webassembly port use the variant's own manifest
(`variants/webassembly/pydevices/manifest.py`), not a preset directly: that
port packages its own asyncio and freezes a Fetch-backed `requests` that must
come first, which the presets' prologue does not know about.
