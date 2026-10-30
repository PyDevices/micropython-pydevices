# Roadmap

Work here mostly follows bug reports and requests, so if something is broken
or missing for you, open an
[issue](https://github.com/PyDevices/micropython-pydevices/issues).

- **Your own firmware from the firmware page.** A build you made, in a
  codespace or anywhere else, goes onto your board from the browser with the
  firmware page's "Install from a file", as the released images do.

More modules in CircuitPython-compatible builds
([what builds today](docs/reference/modules.md#which-modules-build-on-circuitpython)):

- **castif casting on CircuitPython's ESP32-P4 builds.** castif and h264enc
  build there and h264enc encodes, but the port has no network until
  CircuitPython drives the boards' ESP32-C6 radio, so a cast can't start yet.
- **More of usbhost on a CircuitPython ESP32-P4 board.** The host enumerates
  and answers a wireless keyboard-and-mouse dongle on the WIFI6 DEV-KIT. Still
  to try there: a drive, a webcam's stream and a phone. Tethering waits for the
  port's network.
- **The USB host drivers on CircuitPython's own `usb.core`**, on the RP2040
  and i.MX: planned in [pydevices](https://github.com/PyDevices/pydevices/blob/main/ROADMAP.md#usb).

tflite and vision stay MicroPython-only: they are esp-vision's modules, not
ours to port.

More models for tflite:

- **YAMNet's full 521-class int8 model in tflite.** The published
  yamnet-mel-int8-tflm conversion scores every class near 0.5 for any input;
  a fresh int8 conversion of Google's YAMNet would run in the same resolver.
- **micro_speech's own int8 audio preprocessor.** It needs TFLM's Signal
  ops, which tflite doesn't register; until then `audiofeatures` makes the
  same features.

picotts, speech on the board:

- **Speech that starts sooner.** `say()` renders a whole sentence before it
  plays, which on an ESP32-S3 is 1 to 4 s of silence first. The engine runs at
  twice real time there, so a short lead would do once its longest step fits.
- **picotts on more ESP32-P4 boards.** It speaks on the ESP32-S3 T-Embed and
  the P4 WIFI6 DEV-KIT (whose app slot is now 6 MB so it fits); the P4 panels
  aren't built or run yet.

Every module, the same kit:

- **Type stubs and `__revision__` everywhere.** castif, h264enc, jpegio, mdns,
  pngio, ppa, tsmux, tflite and vision get a `.pyi` for your editor and a
  `__revision__` naming the commit your firmware built; tflite and vision a README.
- **One pin file.** `UPSTREAM`, `modules.lock`, `deps.lock` and esp-vision's
  `ESP_VISION.lock` become one `DEPENDENCIES.lock`: a dependency, its URL and
  its full commit on each line.

## Module roadmaps

The modules plan their own work; each roadmap is next to its code.

- [`audioif`](modules/audioif/ROADMAP.md), the audio hardware layer
- [`cameraif`](modules/cameraif/ROADMAP.md), the MIPI-CSI camera
- [`displayif`](modules/displayif/ROADMAP.md), the display buses and drivers
- [`usbhost`](modules/usbhost/ROADMAP.md), the USB host
