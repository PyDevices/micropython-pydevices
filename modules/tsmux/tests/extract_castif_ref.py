"""Regenerate castif_ref.inc: castif's muxer functions, verbatim from git.

    python extract_castif_ref.py [COMMIT]     (default ccccb28, the last castif
                                               with its own muxer)
"""

import os
import re
import subprocess
import sys

COMMIT = sys.argv[1] if len(sys.argv) > 1 else "ccccb28"
NAMES = ["crc32_mpeg", "build_section", "build_tables", "ts_header", "pts_field",
         "mux_video", "mux_lpcm", "emit_tables", "mux_pcr_only"]
HERE = os.path.dirname(os.path.abspath(__file__))

src = subprocess.check_output(["git", "show", COMMIT + ":modules/castif/src/mod_castif.c"],
                              cwd=HERE, text=True)
out = []
for name in NAMES:
    start = re.search(r"^static [^\n]*\b" + name + r"\(", src, re.M).start()
    depth = 0
    for j in range(src.index("{", start), len(src)):
        depth += {"{": 1, "}": -1}.get(src[j], 0)
        if src[j] == "}" and depth == 0:
            out.append(src[start:j + 1])
            break
header = """// castif's MPEG-TS muxer as it was at micropython-pydevices %s
// (modules/castif/src/mod_castif.c), extracted verbatim by
// tests/extract_castif_ref.py. compare_castif.c builds it beside tsmux_core.c
// and checks both write the same packets. Do not edit: regenerate.

""" % COMMIT
with open(os.path.join(HERE, "castif_ref.inc"), "w") as f:
    f.write(header + "\n\n".join(out) + "\n")
print("%d functions from %s" % (len(out), COMMIT))
