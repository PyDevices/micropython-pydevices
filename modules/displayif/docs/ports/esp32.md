# esp32 port

ESP-IDF display interfaces for MicroPython `esp32` port / CircuitPython `espressif` port.

## Native modules

| C source | Python import | SoC | `displaydev` backend |
|----------|---------------|-----|-------------------|
| `mod_dotclockframebuffer.c` | `dotclockframebuffer.DotClockFramebuffer` | RGB LCD (`SOC_LCD_RGB_SUPPORTED`) | **FBDisplay** |
| `mod_i80bus.c` | `i80bus.I80Bus` | S3 (`SOC_LCD_I80_SUPPORTED`) | bus driver |
| `mod_spibus.c` | `spibus.SPIBus` | every ESP32 (`esp_lcd` SPI, DMA) | bus driver |
| `mod_qspibus.c` | `qspibus.QSPIBus` | S3 (`esp_lcd` SPI `quad_mode`) | bus driver |
| `mod_mipidsi.c` | `mipidsi.Bus` / `mipidsi.Display` | P4 (`SOC_MIPI_DSI_SUPPORTED`) | **FBDisplay** |
| `rgbmatrix_pm.c` + common `rgbmatrix` | `rgbmatrix.RGBMatrix` | S3 (Protomatter + LCD_CAM) | **FBDisplay** |

`I80Bus` kwargs match CircuitPython `ParallelBus`: exactly one of `data0` or
`data_pins`, plus `command`, `chip_select`, `write`, optional `read` / `reset`,
and `frequency` (default 30 MHz). `data0` expands to eight consecutive GPIOs.
`frequency` is wired into esp_lcd (`pclk_hz`). Optional `read` is accepted for
CP signature parity but unused on this port (write-only path); **mimxrt**
wires `read` into FlexIO `RDPinIndex`.

On SoCs without the matching peripheral, modules import but constructors raise `NotImplementedError`.

Pin arguments accept `machine.Pin` objects, integers, or port pin-name strings (via `displayif_pin_resolve`).

### `spibus.SPIBus` (DMA)

On ESP32 targets `spibus.SPIBus` is built on ESP-IDF's `esp_lcd` SPI panel IO
and sends pixel data by DMA. The constructor takes the same keywords as the
common bus, so a board config written for any port runs unchanged:

```python
spibus.SPIBus(
    *, id=2, baudrate=24_000_000, polarity=0, phase=0, bits=8,
    lsb_first=False, soft=False, sck=-1, mosi=-1, miso=-1,
    command=-1, chip_select=-1, reset=-1, dma=None,
)
```

`send(command, data)` puts the same bytes on the wire as the blocking bus: the
command byte with D/C low, then the data with D/C high, chip select held across
both, most significant bit first unless `lsb_first`. Reset is pulsed on
construct and by `reset()`, as before. What changes is when `send()` returns:

| Data | What `send()` does |
|------|--------------------|
| None, or up to 64 bytes | Waits for any transfer still going, then sends at once and returns when it's out |
| More than 64 bytes | Waits for any transfer still going, copies the data into the bus's DMA buffer, queues it and returns. The panel keeps receiving while your program runs |
| More than the DMA buffer holds | Streams through the buffer a chunk at a time, and returns once the last chunk is queued |

The DMA buffer is internal RAM: 128 KB on the ESP32-P4, which holds a
240x240 RGB565 frame (115,200 bytes) whole, and 32 KB on other chips. It is
halved while internal RAM is short. The constructor prints the size it got.
Data that fits goes as one transfer with chip select held. Larger data goes in
16 KB chunks, and chip select is released between them; ST7789 and ILI9341
panels resume the pixel write when it's asserted again.

Your buffer is yours again as soon as `send()` returns, so you can draw the
next frame into it straight away. Three methods report on the transfer:

| Method | Returns |
|--------|---------|
| `busy()` | True while a queued transfer is still going |
| `wait()` | None, once every queued transfer is done. Raises `OSError(ETIMEDOUT)` after a second without progress |
| `register_callback(fn)` | Calls `fn()` after every `send()` that carried data, when your buffer is free. `None` removes it |

LVGL needs nothing new. `display_driver` marks a flush ready when
`blit_rect()` returns, which is when the bus has its own copy, and LVGL renders
the next area while the DMA sends this one. The next flush's window command
waits for the transfer ahead of it. `DisplayDriver(..., blocking=False)` works
too: it registers `flush_ready` with `register_callback`, which the bus calls
at the same moment. A callback scheduled from the transfer-done interrupt was
the other choice, and it would hang LVGL: LVGL waits for a flush in a C loop
that never lets a scheduled Python callback run.

The bus uses the blocking `machine.SPI` bus instead, with the same methods
(`busy()` is always False and `wait()` returns at once), when:

- `soft=True`, there's no `command` pin (9-bit mode), or `bits` isn't 8;
- `sck` or `mosi` isn't given;
- `miso` is given, because a display bus with MISO is usually shared through
  `machine.SPI` with a touch controller or an SD card, and `machine.SPI` can't
  share a host that `esp_lcd` owns;
- `machine.SPI` already holds the host, or internal RAM can't hold the buffer.

`dma=True` asks for DMA whatever `miso` says, and raises if it can't have it;
`dma=False` keeps the blocking bus. While waiting for a transfer the bus
releases the GIL, so other threads run. `deinit()`, `__del__` and a soft reset
wait for queued transfers (up to a second), then free the panel IO, the SPI
host and the buffers.

#### Measuring it

Frames per second of a full-panel `blit_rect` loop, and how long each call
holds your program, with no other work and with 30 ms of work per frame. Run
it once on firmware with the blocking bus and once with this one. With
`display_drv` from the board config:

```python
import time
from board_config import display_drv as d

buf = memoryview(bytearray(d.width * d.height * 2))
w = getattr(d.display_bus, "wait", lambda: None)

def run(n=50, work_ms=0):
    held = 0
    t0 = time.ticks_us()
    for _ in range(n):
        t = time.ticks_us()
        d.blit_rect(buf, 0, 0, d.width, d.height)
        held += time.ticks_diff(time.ticks_us(), t)
        if work_ms:
            time.sleep_ms(work_ms)
    w()
    total = time.ticks_diff(time.ticks_us(), t0)
    print("work %d ms: %.1f fps, blit_rect %.1f ms, free %.1f ms per frame"
          % (work_ms, n * 1e6 / total, held / n / 1000, (total - held) / n / 1000))

run(); run(work_ms=30)
```

On a 240x240 panel at 20 MHz the blocking bus holds your program for the whole
frame, about 46 ms, so 30 ms of work drops it to about 13 fps. With DMA,
`blit_rect` holds it only for the copy, and the frame rate stays near the wire
limit until the work per frame passes the wire time.

### `qspibus.QSPIBus` (ESP32-S3)

Keyword-only constructor matches CircuitPython `qspibus.QSPIBus`:

```python
qspibus.QSPIBus(
    *,
    clock, data0, data1, data2, data3, cs,
    dcx=None, reset=None, frequency=80_000_000,
)
```

Uses ESP-IDF `spi_bus_initialize` + `esp_lcd_new_panel_io_spi` with `quad_mode`, dual DMA bounce buffers, and encoded QSPI command words (`0x02` / `0x32`). Methods: `send(command, data)`, `write_command`, `write_data`, `reset`, `deinit` / `__del__`. Soft-reset tears down panel IO, SPI host, DMA buffers, and the transfer semaphore before GC.

### `dotclockframebuffer.DotClockFramebuffer` (RGB LCD / Qualia)

Python import is `dotclockframebuffer.DotClockFramebuffer` (same as CircuitPython).
Required RGB565 pin tuples (`red`/`green`/`blue` = 5/6/5), matching CircuitPython
(wire order B0..B4, G0..G5, R0..R4).

Behavioral contract (proven on Qualia S3 + TL040HDS20):

| Topic | Behavior |
|-------|----------|
| Scanout | Continuous DMA (`refresh_on_demand=0`); panel-owned FB |
| `refresh()` | Double-buffered: promote the back buffer, wait for the scanout to take it (up to a frame), then paint the other buffer. Single buffer: cache writeback only |
| `present()` / `wait()` | `refresh()` in two halves: `present()` promotes and returns at once, `wait()` returns once the scanout has the new buffer. A GUI that renders straight into `framebuffers()` (LVGL DIRECT) calls `wait()` before it paints again, and gets the frame period back instead of blocking in every present. `blit`, `fill_rect`, `framebuffers` and `refresh` finish a pending flip themselves |
| Large panels | Bounce buffers `bounce_rows * h_res` px each (`bounce_rows=`, 4..20, default 20; stepped down if internal DMA RAM will not hold two) + dirty-row msync |
| Buffer protocol | typecode `'B'` (MP has no `memoryview.cast`) |
| Fast paths | Native `blit` / `fill_rect` (expose via custom `attr`) |
| Lifecycle | Idempotent `deinit` / `__del__` / ctor + soft-reset teardown |

Both bounce buffers come out of internal DMA RAM, which Wi-Fi, hardware AES
and a USB host need too: an 800-wide panel takes 64 KB of it at 20 rows. A
board that runs those beside the panel asks for fewer, e.g.
`DotClockFramebuffer(..., bounce_rows=10)` on the Waveshare
ESP32-S3-Touch-LCD-7 (32 KB). Fewer rows mean more refill interrupts per
frame, not less throughput: the same bytes are copied either way.

See [soft-reset-and-bring-up.md](../soft-reset-and-bring-up.md#reference-dotclockframebufferdotclockframebuffer-on-qualia-esp32-s3).

### `mipidsi` (ESP32-P4 DSI)

`mipidsi.Bus` + `mipidsi.Display` for `SOC_MIPI_DSI_SUPPORTED`. Proven on
Waveshare / Espressif P4 4B touch LCD with LVGL soft-reset re-import.

`Bus(frequency=500_000_000, num_lanes=2, …)` matches CircuitPython defaults
(`ldo_*` remain displayif extras).

`Display(bus, init_sequence, *, …)` takes positional `bus` and `init_sequence`
like CircuitPython. Optional kwargs: `virtual_channel=0` (wired into esp_lcd
DBI/DPI), `color_depth=16`, and `non_burst=False` / `continuous_clock=False`.
Set both of those to `True` for a panel behind a DSI-to-RGB bridge such as the
Chipone ICN6211 on Raspberry Pi-style DSI displays: ESP-IDF otherwise sends
burst-mode video with an idling clock lane, and the bridge shows nothing (a
white panel). They reach a private ESP-IDF struct checked against v5.5.4 only,
and raise on any other ESP-IDF. Panel **reset** and **backlight** GPIO are owned
by board_config (not `Display`). CircuitPython-only kwargs (`rotation`,
`brightness`, `backlight_pin`, `backlight_on_high`, `native_frames_per_second`)
are not accepted.

| Topic | Behavior |
|-------|----------|
| `refresh()` | Full-FB `esp_cache_msync` + `esp_lcd_panel_draw_bitmap` |
| `refresh_rect(x, y, w, h)` | Dirty-row msync **+ banded `draw_bitmap` of those rows**. Widened to full rows: `row_stride == width * 2`, so a band is contiguous and needs no staging copy |
| Scanout | **On-demand, not continuous.** Every frame needs a `draw_bitmap`; the DPI engine does not keep re-reading the buffer. Contrast `DotClockFramebuffer`, which *is* continuous DMA — see below |
| Fast path | Native `Display.blit` + dirty-row msync (Python row slices WDT) |
| Buffer protocol | typecode `'B'` |
| Lifecycle | BSS host mirror; `esp_lcd_del_*` + LDO + SPIRAM free on soft-reset |
| Methods | Custom `attr` exports `refresh` / `blit` / `deinit` / `__del__` |

> **Do not copy `DotClockFramebuffer`'s continuous-DMA scanout model here.**
> A cache sync alone does not present on DSI. `refresh_rect` once synced the
> cache only, on the assumption the DPI engine kept scanning the framebuffer;
> it does not, so a GUI rendering straight into the shared framebuffer painted
> one frame and then froze. Verified on a 720x720 P4 panel: a UI stuck on its
> first frame animated as soon as a `draw_bitmap` ran per frame.

See [soft-reset-and-bring-up.md](../soft-reset-and-bring-up.md) and
[idempotent-lifecycle.md](../idempotent-lifecycle.md).

Large framebuffers use **PSRAM** when available — see [port-matrix.md](../port-matrix.md#esp32-psram--sdkconfig-large-framebuffers).

## Build

`micropython.mk` and `micropython.cmake` under `src/ports/esp32/` are included when building the esp32 port.
