"""Structural checks of a USB configuration descriptor, as a host receives it.

    python3 desc_check.py HEX SPEED IN_MAX         # HEX: the configuration bytes
    python3 desc_check.py --plant                  # shows a planted fault is caught

``faults(cfg, speed, in_max)`` returns a list of what's wrong, empty when the
descriptor is sound. It checks what a host checks and what our firmware has
got wrong before: wTotalLength; bNumInterfaces against the interfaces
present and numbered from 0 without gaps; each interface's bNumEndpoints
against the endpoints that follow it; endpoint addresses used once; IN
endpoints within the controller's FIFOs (in_max); bulk endpoints 64 bytes at
full speed and 512 at high speed; interrupt intervals in range for the speed;
and every interface association pointing at interfaces that exist. The
proof matrix runs it on the descriptor Windows received for every
combination (``usb_desc.py --raw``).
"""
import json
import sys


def faults(cfg, speed, in_max):
    out = []
    if len(cfg) < 9 or cfg[1] != 2:
        return ["not a configuration descriptor"]
    total = cfg[2] | cfg[3] << 8
    if total != len(cfg):
        out.append("wTotalLength %d, but %d bytes" % (total, len(cfg)))
    declared = cfg[4]
    itfs = {}       # (number, alternate) -> [declared endpoints, seen]
    current = None
    eps = set()
    iads = []
    i = cfg[0]
    while i + 2 <= len(cfg):
        n, kind = cfg[i], cfg[i + 1]
        if n < 2 or i + n > len(cfg):
            out.append("descriptor at %d has length %d" % (i, n))
            break
        if kind == 4 and n >= 9:
            current = (cfg[i + 2], cfg[i + 3])
            itfs[current] = [cfg[i + 4], 0]
        elif kind == 11 and n >= 8:
            iads.append((cfg[i + 2], cfg[i + 3]))
        elif kind == 5 and n >= 7:
            addr, attr = cfg[i + 2], cfg[i + 3] & 3
            mps = cfg[i + 4] | cfg[i + 5] << 8
            interval = cfg[i + 6]
            if current is None:
                out.append("endpoint 0x%02x outside any interface" % addr)
            else:
                itfs[current][1] += 1
            # The same address may be reused by another alternate setting of
            # the same interface, never by two interfaces.
            key = (addr, current[0] if current else None)
            if any(a == addr and k != key[1] for a, k in eps):
                out.append("endpoint 0x%02x used by two interfaces" % addr)
            eps.add(key)
            if addr & 0x80 and (addr & 0x7F) > in_max:
                out.append("IN endpoint %d past the controller's %d" % (addr & 0x7F, in_max))
            if attr == 2:
                want = 512 if speed == "high" else 64
                if mps != want:
                    out.append("bulk endpoint 0x%02x is %d bytes at %s speed, not %d" % (addr, mps, speed, want))
            elif attr == 3:
                if speed == "high" and not 1 <= interval <= 16:
                    out.append("interrupt endpoint 0x%02x interval %d out of 1..16 at high speed" % (addr, interval))
                if speed != "high" and interval == 0:
                    out.append("interrupt endpoint 0x%02x interval 0" % addr)
        i += n
    numbers = sorted({k[0] for k in itfs})
    if numbers != list(range(len(numbers))):
        out.append("interfaces numbered %s, not 0..%d" % (numbers, len(numbers) - 1))
    if declared != len(numbers):
        out.append("bNumInterfaces %d, but %d interfaces" % (declared, len(numbers)))
    for (num, alt), (want, seen) in sorted(itfs.items()):
        if want != seen:
            out.append("interface %d alt %d declares %d endpoints, has %d" % (num, alt, want, seen))
    for first, count in iads:
        if any(n not in numbers for n in range(first, first + count)):
            out.append("association %d+%d names a missing interface" % (first, count))
    return out


def main():
    if sys.argv[1:] == ["--plant"]:
        # A CDC association, its two interfaces and three endpoints, sound;
        # then the same with the data interface's bNumEndpoints wrong.
        sample = bytearray.fromhex(
            "0902" "3e00" "02" "01" "00" "80" "32"
            "080b" "00" "02" "02" "02" "00" "00"
            "0904" "00" "00" "01" "02" "02" "00" "00"
            "0705" "81" "03" "0800" "10"
            "0904" "01" "00" "02" "0a" "00" "00" "00"
            "0705" "02" "02" "4000" "00"
            "0705" "82" "02" "4000" "00")
        sample[2] = len(sample)
        clean = faults(bytes(sample), "full", 4)
        bad = bytearray(sample)
        bad[9 + 8 + 9 + 7 + 4] = 3  # data interface claims 3 endpoints
        planted = faults(bytes(bad), "full", 4)
        print(json.dumps({"clean": clean, "planted": planted}))
        sys.exit(0 if not clean and planted else 1)
    cfg = bytes.fromhex(sys.argv[1])
    found = faults(cfg, sys.argv[2], int(sys.argv[3]))
    print(json.dumps({"faults": found}))
    sys.exit(1 if found else 0)


if __name__ == "__main__":
    main()
