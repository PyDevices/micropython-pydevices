# Console captures

`p4_drum_machine_panic_2026-09-27.log`: the P4 rebooting a few seconds after
Start in the stock drum machine, no cast running (castif in the firmware but
never initialised). `mpftp monitor COM4` read-only capture. Decoded against
`build-CAST_P4-PRE_REV3_C6_WIFI/micropython.elf` (build 5, 19:27):

- `Guru Meditation Error: Core 0 panic'ed (Instruction access fault)`,
  MEPC 0x00000000: a call through a NULL function pointer, on the interrupt
  stack.
- RA 0x401e8602 = `xPortEnterCriticalTimeoutSafe` (portmacro.h:658);
  S1 0x401a9590 = `machine_timer_isr_virtual` (ports/esp32/machine_timer.c),
  which calls `self->handler(self)` on a virtual `machine.Timer`.
- The board's `/lib/multimer` was replaced with pydevices main 0.6.3 that
  evening (the frozen LVGL `display_driver` needs `app.pause_polling`, whose
  appdev needs `multimer._hostloop`); its machine source re-arms a one-shot
  `machine.Timer` on every tick.
