# Draft issue: esp32 WebREPL Ctrl-C can't break a loop that never waits

A draft for Brad to post on micropython/micropython. Nothing here has been
posted. The fix we carry meanwhile is
[patches/0011](../../patches/0011-micropython-esp32-webrepl-ctrl-c-in-tight-loops.patch).

---

**Title:** esp32: Ctrl-C over WebREPL doesn't interrupt a loop that never waits, and blocks new WebREPL connections

**Port, board, firmware**

esp32 port, MicroPython v1.29.0. Seen on an ESP32-P4 board (C6 Wi-Fi
companion); the code involved is port-wide, so any esp32 build with WebREPL
should behave the same.

**Reproduction**

1. Start WebREPL (`import webrepl; webrepl.start()`) and connect with the
   WebREPL client.
2. Run `while True: pass`.
3. Press Ctrl-C. Nothing happens; the loop keeps running.
4. Open a second WebREPL client and connect. It hangs instead of being
   told "Concurrent WebREPL connection ... rejected".

The same Ctrl-C over the USB/UART REPL stops the loop at once. Over WebREPL,
a loop that sleeps (`while True: time.sleep_ms(1)`) can be interrupted, so
this is specific to code that never waits.

**What happens**

Serial Ctrl-C arrives by interrupt: `ports/esp32/uart.c:116` and
`ports/esp32/usb_serial_jtag.c:58` call `mp_sched_keyboard_interrupt()` from
their ISRs, and the VM raises it at its next branch
(`py/vm.c:1341`, `pending_exception_check`).

WebREPL's bytes sit in a socket until something reads them. On esp32 that is
`socket_events_handler()` (`ports/esp32/modsocket.c:123`), which selects on
the sockets registered with `setsockopt(SOL_SOCKET, 20, callback)` and runs
their callbacks. For the WebREPL client the callback is `os.dupterm_notify`
(`extmod/modos.c:134`), which reads through dupterm, where the interrupt
character is recognised and `mp_sched_keyboard_interrupt()` is called
(`extmod/os_dupterm.c:157-159`).

The port calls `socket_events_handler()` only from `MICROPY_EVENT_POLL_HOOK`
(`ports/esp32/mpconfigport.h:343-371`) and from `mp_hal_delay_ms()`
(`ports/esp32/mphalport.c:203`), i.e. only while the program sleeps, waits
or sits at the REPL. A loop that never waits never reaches either, so the
Ctrl-C byte is never read. The listen socket's accept callback is starved the
same way, which is why a new connection hangs.

**Related**

#11536 was the same symptom on the Pico W, fixed for rp2 by #13200. I couldn't
find an esp32 issue.

**A possible fix**

Give the esp32 port a `MICROPY_VM_HOOK_LOOP` that, every N branches (a
decrement and test per branch), checks whether any socket has an events
callback and, rate-limited by `mp_hal_ticks_ms()` (say every 20 ms), schedules
the existing socket poll on a static scheduler node
(`mp_sched_schedule_node`). Scheduling rather than calling from the hook keeps
the callbacks, which can be Python (webrepl's accept handler is), in the same
context every scheduled callback gets: main thread, scheduler locked,
pending exceptions raised after they return. The cost with WebREPL off is a
decrement per branch and a call every N branches that returns at once.

We carry this as a small patch on v1.29.0 and it builds for the ESP32-P4;
happy to open it as a PR if the approach is acceptable. Native and viper code
doesn't run VM hooks, so it wouldn't be covered, as on other ports.
