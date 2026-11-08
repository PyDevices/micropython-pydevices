# Build with a few instruments

`audioinstruments` is large: on a 4 MB ESP32, the whole package doesn't fit
beside LVGL and the audio modules. Name the instruments you play in
`AUDIOINSTRUMENTS`, and the build freezes only those and what they import:

```bash
AUDIOINSTRUMENTS=electric_piano,mono_synth,rhythm808 ./build_mp.py --port esp32 --board ESP32_GENERIC --variant SPIRAM --flash 4MB --modules pydevices,displayif,lvgl,audioif,audiodsp,ulab,audioinstruments
```

On the board, `audioinstruments.ALL` lists just those three, so an app that
offers every instrument in it, such as Synth Station, offers those three. The
rest of the package isn't there, and importing one of its instruments raises
`ImportError`.

A name the package doesn't have stops the build, and the message lists every
name it does have. The names are the modules in
`audioinstruments.ALL` and `audioinstruments.SAMPLERS`. Leave
`AUDIOINSTRUMENTS` unset, or empty, for the whole package.

## What it saves

Compiled, the whole package is 274 files and 870 KB. The three instruments
above, with the rack and panel they share, are 82 files and 253 KB.

With the modules in the command above, the whole package makes a 3,965 KB
image, which no partition table on a 4 MB flash can hold. With the three
instruments, it's 3,399 KB, and
[the build grows the app partition](fit-firmware-on-a-small-flash.md) to fit
it, leaving a 448 KB filesystem.

## On the filesystem instead

On a board with a bigger filesystem, you can keep the instruments out of the
firmware and copy them on as `.mpy` files. Build without `audioinstruments`,
then write the selection and copy it:

```bash
python modules/audioinstruments/selection.py electric_piano,mono_synth,rhythm808 --out build-instruments --mpy-cross micropython/mpy-cross/build/mpy-cross
mpftp put -r build-instruments/audioinstruments /lib/audioinstruments
```

The script reads the package a build put under `deps/omnipython-lib`, which
any build that names `pydevices` or another of
[omnipython-lib's modules](choose-modules.md#python-packages-from-omnipython-lib)
makes. The instruments are bytecode only, so the `.mpy` files run on any board
with firmware from the same build tree.

A LittleFS filesystem gives every file at least one 4 KB block, so those three
instruments, 82 files and 253 KB of `.mpy`, take about 480 KB of it, where
frozen they add 242 KB to the image. Built without `audioinstruments`, the
command above makes a 3,157 KB image and leaves a 704 KB filesystem, so they
fit there with room for little else. When flash is tight, freeze them.
