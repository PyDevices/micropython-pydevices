# Reference

This repository has no Python package to document, so these pages are tables:
what `build_mp.py` takes, the modules you can name, the boards and variants
built, and the files that pin what a build uses.

- [The build command](build-command.md): every option, environment variable and
  where output goes.
- [Modules](modules.md): the short names `--modules` takes, and which build on
  CircuitPython.
- [Boards and variants](boards.md): the command for each board, and every variant
  in `variants/` and `boards/`.
- [Files and pins](files.md): `UPSTREAM`, the lock files, the patch series and
  the scripts.

```{toctree}
:hidden:

build-command
modules
boards
files
```
