# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Compare board_check.py's records: what differs from the driver's.

    python3 board_compare.py bc_ov2710_driver.json bc_ov2710_description.json [more ...]
"""

import json
import sys


def compare(base: dict, other: dict) -> list[str]:
    diffs = []
    if len(base["modes"]) != len(other["modes"]):
        diffs.append(f"modes: {len(base['modes'])} against {len(other['modes'])}")
    for a, b in zip(base["modes"], other["modes"]):
        k = a["index"]
        if a["size"] != b["size"] or a["sensor"] != b["sensor"]:
            diffs.append(f"mode {k}: {a['sensor']} {a['size']} against {b['sensor']} {b['size']}")
        if a["controls"] != b["controls"]:
            diffs.append(f"mode {k}: controls {a['controls']} against {b['controls']}")
        for reg in sorted(set(a["regs"]) | set(b["regs"])):
            if a["regs"].get(reg) != b["regs"].get(reg):
                diffs.append(f"mode {k} {reg}: {a['regs'].get(reg)} against {b['regs'].get(reg)}")
        for state in sorted(set(a["states"]) | set(b["states"])):
            sa, sb = a["states"].get(state, {}), b["states"].get(state, {})
            for reg in sorted(set(sa) | set(sb)):
                if sa.get(reg) != sb.get(reg):
                    diffs.append(f"mode {k} {state} {reg}: {sa.get(reg)} against {sb.get(reg)}")
    return diffs


def main() -> None:
    base = json.load(open(sys.argv[1]))
    n_regs = sum(len(m["regs"]) for m in base["modes"])
    n_states = sum(len(m["states"]) for m in base["modes"])
    print(f"{sys.argv[1]}: {len(base['modes'])} modes, {n_regs} mode registers, {n_states} control states")
    for path in sys.argv[2:]:
        diffs = compare(base, json.load(open(path)))
        print(f"{path}: " + ("registers equal" if not diffs else f"{len(diffs)} differ"))
        for d in diffs[:40]:
            print("   ", d)
    sys.exit(0)


if __name__ == "__main__":
    main()
