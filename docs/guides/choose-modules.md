# Choose modules

`--modules` takes a comma-separated list:

```bash
./build_mp.py --port rp2 --board RPI_PICO2 --modules displayif,jpegio,pygraphics,palettes,pdwidgets
```

A name is one of three things:

- **A short name** from [the modules table](../reference/modules.md):
  `displayif`, `pygraphics`, `pydevices`.
- **A path**, to a module of your own: a directory with a `manifest.py`, or a
  C module with its `micropython.mk` or `micropython.cmake`.
- **`all`**: every module except the opt-in ones. `tflite` and `vision` are
  opt-in (each has an `OPT_IN` file saying why), so name them to get them:
  [Use esp-vision's modules](use-esp-vision-modules.md).

A module never brings another, and nothing checks dependencies. If you name
`audiocomponents`, `tflite` or `vision`, name `ulab` too, or use `all`.

## Python packages from omnipython-lib

Our Python-only packages come from omnipython-lib: `pydevices`, `palettes`,
`pdwidgets` and `audiocomponents` are modules whose manifest requires them from
it. Any other package of it, such as `appkit` or a board config, can be named
too.

## The desktop board config

The desktop `pydevices` variants freeze a board config that imports
`displaydev`. `displaydev` is frozen only when `--modules` names `pydevices` or
`all`, so without one of them `import board_config` fails:

```bash
./build_mp.py --port unix --variant pydevices --modules pydevices,displayif,pygraphics
```

## A smaller image

`all` is large. On a small flash, name only what your project uses:
[Fit firmware on a small flash](fit-firmware-on-a-small-flash.md).
