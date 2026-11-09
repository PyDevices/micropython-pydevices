# Build with a few instruments and effects

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

## A few effects

`audioeffects` works the same way. Name the effects you use in
`AUDIOEFFECTS`, by module or class name, and the build freezes only those,
the package's `__init__` and the base they share, and any effect they build
on:

```bash
AUDIOEFFECTS=chorus,reverb ./build_mp.py --port esp32 --board ESP32_GENERIC --variant AUDIO_KIT --flash 4MB --modules pydevices,displayif,lvgl,btclassic,audioif,audiodsp,audioeffects
```

On the board, `audioeffects.ALL` lists `Chorus` and `Reverb`, and
`audioeffects.create("Compressor", ...)` raises `ImportError`. Name a rack,
such as `ShimmerHall`, and the effects it's made of come with it. An unknown
name stops the build and lists the effects the package has. Leave
`AUDIOEFFECTS` unset, or empty, for all of them.

Only an `audioeffects` that loads each effect the first time it's used can be
frozen in part, which it does from 0.5.2.dev0. With an older one, the build
stops and says so, rather than freezing a package that fails to import.

Compiled, the whole package is 49 files and 278 KB. Chorus and reverb are 4
files and 23 KB. On an ESP32 Audio Kit, with the modules above, `soundfont`
from `audioinstruments` and a General MIDI sound module's libraries, the
whole package makes a 4,095 KB image, which no partition table on a 4 MB flash
can hold. With the two effects it's 3,843 KB, 252 KB less, and fits the kit's
3,904 KB app partition beside a 128 KB filesystem.

`modules/audioeffects/selection.py` writes the same selection as files, with
the same `--out` and `--mpy-cross` options as the instruments' script, for a
board with room on its filesystem.
