# Spike tools

- `tsdump.py FILE.ts ...`: prints the packet structure of a transport stream
  (tables, adaptation fields, PES headers, PTS against PCR). It found the
  missing `last_section_number` byte that Windows would not forgive and ffmpeg
  did. Run it on any stream before blaming clocks.
- `p4_recv_file.py` (board) and `push_file.py` (PC): move a file onto the P4 over
  Wi-Fi at a few hundred kbit/s, with the board answering its own size and
  SHA-256 so the sender knows when the flash write has finished. Touching the
  board with mpftp before that answer interrupts the write and truncates the
  file.
- `mice_source.py`: the Windows-Python reference source. It never got a
  call-back, because Windows refuses a source on its own machine; kept as the
  record of the protocol and of that dead end.
