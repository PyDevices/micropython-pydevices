# AGENTS.md

- Run `python3 tools/check_qstr_reachability.py` after you touch a `micropython.mk` or CMake file. Never put an `MP_QSTR_` name behind a define this usermod supplies: unix stays green and the real port fails with `'MP_QSTR_<name>' undeclared` ([why](docs/qstrs-and-usermod-defines.md)).
- Before you change lifecycle, soft-reset or a board bring-up, read [idempotent-lifecycle.md](docs/idempotent-lifecycle.md) and [soft-reset-and-bring-up.md](docs/soft-reset-and-bring-up.md). The `mipidsi` and `dotclockframebuffer` bring-ups are the reference; keep their scanout, blit, attribute and soft-reset patterns when you change a sibling.
- Anything that owns DMA, IRQ, PIO or SDK handles registers with `displayif_register_soft_reset()` and tears down on the same path as `deinit` and `__del__`. Teardown runs before the heap is wiped, so release host resources and never `m_free` GC memory.
- Don't patch the `micropython/` tree for soft-reset teardown; use a usermod `--wrap` or the registry. Fix root causes here rather than special-casing a board config.
- After any teardown change, check on a board: construct, soft-reset, construct again. It must succeed without a hard reset ([the checklist](docs/soft-reset-and-bring-up.md)).
- Only `usdl2` builds for CircuitPython (unix, through the root `micropython.mk`); MCU interfaces stay on stock CircuitPython. The module and port map is [docs/port-matrix.md](docs/port-matrix.md).
- A frozen module wins over `/lib` on both interpreters, so a changed display module never runs on a board until you rebuild the firmware.
