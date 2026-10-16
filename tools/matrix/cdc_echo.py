"""Echo a pattern through a board's extra USB serial port (Windows Python).

    python cdc_echo.py SERIAL [BYTES]

Finds the board's serial ports by USB serial number, skips the first
interface (the board's own console), writes BYTES of a pattern to the next
one and reads it back. Prints a JSON line; "pass" is true when every byte
came back in order.
"""
import json
import sys
import time

import serial
from serial.tools import list_ports

want = sys.argv[1].upper()
size = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
ports = sorted(
    (p for p in list_ports.comports() if p.serial_number and want in p.serial_number.upper()),
    key=lambda p: p.location or "",
)
extra = [p for p in ports if not (p.location or "").endswith("x.0")]
if not extra:
    print(json.dumps({"pass": False, "error": "no extra serial port", "ports": [p.device for p in ports]}))
    sys.exit(1)
dev = extra[0].device
pattern = bytes((i * 7) & 0xFF for i in range(size))
got = bytearray()
with serial.Serial(dev, 115200, timeout=0.1) as s:
    s.reset_input_buffer()
    for i in range(0, size, 64):
        s.write(pattern[i : i + 64])
        time.sleep(0.005)
    end = time.time() + 5
    while len(got) < size and time.time() < end:
        got += s.read(size - len(got))
ok = bytes(got) == pattern
print(json.dumps({"pass": ok, "port": dev, "sent": size, "received": len(got)}))
sys.exit(0 if ok else 1)
