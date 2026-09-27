# Draft issue: esp32 machine.Timer re-armed while it fires calls NULL

A draft for Brad to post on micropython/micropython. Nothing here has been
posted. The fix we carry meanwhile is
[patches/0016](../../patches/0016-micropython-esp32-machine-timer-fire-during-reinit.patch),
and the tracking issue is micropython-pydevices#14.

---

**Title:** esp32: re-initialising a machine.Timer while it fires panics (Instruction access fault, MEPC 0)

**Port, board, firmware**

esp32 port, MicroPython v1.29.0, ESP-IDF v5.5.4. Seen on an ESP32-P4 (the
Waveshare ESP32-P4-WIFI6-Touch-LCD-4B, pre-rev-3 silicon, C6 Wi-Fi
companion) and earlier on an ESP32-S3. The code involved is port-wide.

**Reproduction**

On a dual-core chip, re-arm a one-shot virtual timer at about the moment it
is due, over and over:

```python
import machine, time

fired = 0

def cb(t):
    global fired
    fired += 1

tim = machine.Timer(-1)
seed = 1
while True:
    tim.init(mode=machine.Timer.ONE_SHOT, period=1, callback=cb)
    seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
    t1 = time.ticks_us()
    while time.ticks_diff(time.ticks_us(), t1) < 700 + seed % 600:
        pass
```

On the ESP32-P4 this panicked in under two seconds in both of our runs. (Our
firmware is v1.29.0 plus a few local patches, none of which touch
`machine_timer.c` or esp_timer.)

```
Guru Meditation Error: Core  0 panic'ed (Instruction access fault). Exception was unhandled.
MEPC    : 0x00000000  RA      : 0x401e8602  SP      : 0x4ff35c70  GP      : 0x4ff1fa80
```

We found it in a timing library that keeps one one-shot `machine.Timer`
and re-arms it to the next deadline of many software timers, so it calls
`init()` thousands of times a minute; an LVGL app on it rebooted the P4
within seconds of starting to play audio.

**What happens**

A virtual timer is an `esp_timer` with `ESP_TIMER_TASK` dispatch
(`ports/esp32/machine_timer.c:148`). Its callback,
`machine_timer_isr_virtual`, runs on the esp_timer task, which is pinned
to core 0 (`CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0=y` in the build), while the
interpreter runs on core 1 (`ports/esp32/mphalport.h:54`). esp_timer takes a timer off
its list under the list lock and then releases the lock before it calls the
callback (`components/esp_timer/src/esp_timer.c:434-435` in ESP-IDF v5.5.4),
so `esp_timer_stop()` (`machine_timer.c:208`) can return while a callback
that has already been dispatched has still to run.

`Timer.init()` on a timer that is running calls `machine_timer_deinit()`
(`machine_timer.c:273`), which stops the timer and then sets
`self->handler = NULL` (`machine_timer.c:242`). The handler is set again only
at `machine_timer.c:298`, after the arguments are parsed. A dispatched
callback that reads the handler in that gap calls it unchecked
(`machine_timer.c:75`) and jumps to address 0. In the panic above, MEPC is 0
and S1 holds `machine_timer_isr_virtual`.

The hardware-timer ISR has the same unchecked call
(`machine_timer.c:65`), and `deinit()` has the same gap for `handler_ctx`
(`machine_timer.c:243`), which `machine_timer_handler` passes to
`mp_sched_schedule` (`machine_timer.c:83`).

**A possible fix**

What we carry, three changes in `ports/esp32/machine_timer.c`:

1. Both callbacks read `self->handler` once and return if it is NULL, and
   `machine_timer_handler` returns if `handler_ctx` is NULL. A fire in
   flight during `deinit()` is dropped, as it would have been a moment later.
2. `init()` on a running timer stops it (`machine_timer_stop()`) instead of
   deinitialising it, so the handler is never NULL on the way to being
   replaced. `machine_timer_configure()` already tolerates an enabled
   gptimer (`ESP_ERR_INVALID_STATE`) and a virtual timer already on the list.
3. `init()` sets `handler_ctx` before `handler`.

A fire dispatched just before `init()` may still deliver the old callback
once, which is also what happens when the fire comes just before `init()` is
called.

With these changes the loop above ran 60 s on the P4 (32693 fires) with no
panic, and the timing library that found it ran 10 minutes (288667 wakes).
Happy to open it as a PR if the approach is acceptable.
