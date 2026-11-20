# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

# offheap: byte buffers outside the garbage-collected heap, for big pixel
# buffers that would otherwise make every collection slow. C only; esp32,
# unix, windows and webassembly.
c_module(".")
